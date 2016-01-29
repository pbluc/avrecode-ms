#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <sstream>
#include <typeinfo>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavcodec/cabac.h"
#include "libavcodec/coding_hooks.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavutil/file.h"
}

#include "recode.pb.h"


// CABAC blocks smaller than this will be skipped.
const int SURROGATE_MARKER_BYTES = 8;


template <typename T>
std::unique_ptr<T, std::function<void(T*&)>> av_unique_ptr(T* p, const std::function<void(T*&)>& deleter) {
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return std::unique_ptr<T, std::function<void(T*&)>>(p, deleter);
}
template <typename T>
std::unique_ptr<T, std::function<void(T*&)>> av_unique_ptr(T* p, void (*deleter)(T**)) {
  return av_unique_ptr<T>(p, [deleter](T*& to_delete){ deleter(&to_delete); });
}
template <typename T>
std::unique_ptr<T, std::function<void(T*&)>> av_unique_ptr(T* p, void (*deleter)(void*) = av_free) {
  return av_unique_ptr<T>(p, [deleter](T*& to_delete){ deleter(to_delete); });
}

template <typename T = std::function<void()>>
struct defer {
  T to_defer;
  explicit defer(const T& to_defer) : to_defer(to_defer) {}
  defer(const defer&) = delete;
  ~defer() { to_defer(); }
};


// Sets up a libavcodec decoder with I/O and decoding hooks.
template <typename Driver>
class decoder {
 public:
  decoder(Driver *driver, const std::string& input_filename) : driver(driver) {
    const size_t avio_ctx_buffer_size = 1024*1024;
    uint8_t *avio_ctx_buffer = static_cast<uint8_t*>(av_malloc(avio_ctx_buffer_size));

    format_ctx = avformat_alloc_context();
    if (avio_ctx_buffer == nullptr || format_ctx == nullptr) throw std::bad_alloc();
    format_ctx->pb = avio_alloc_context(
        avio_ctx_buffer,                      // input buffer
        avio_ctx_buffer_size,                 // input buffer size
        false,                                // stream is not writable
        this,                                 // first argument for read_packet()
        read_packet,                          // read callback
        nullptr,                              // write_packet()
        nullptr);                             // seek()

    if (avformat_open_input(&format_ctx, input_filename.c_str(), nullptr, nullptr) < 0) {
      throw std::invalid_argument("Failed to initialie decoding context: " + input_filename);
    }
  }
  ~decoder() {
    for (int i = 0; i < format_ctx->nb_streams; i++) {
      avcodec_close(format_ctx->streams[i]->codec);
    }
    av_freep(&format_ctx->pb->buffer);  // May no longer be the same buffer we initially malloced.
    av_freep(&format_ctx->pb);
    avformat_close_input(&format_ctx);
  }

  // Read enough frames to display stream diagnostics. Only used by compressor,
  // because hooks are not yet set. Reads from already in-memory blocks.
  void dump_stream_info() {
    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
      throw std::runtime_error("Invalid input stream information.\n");
    }
    av_dump_format(format_ctx, 0, format_ctx->filename, 0);
  }

  // Decode all video frames in the file in single-threaded mode, calling the driver's hooks.
  void decode_video() {
    auto frame = av_unique_ptr(av_frame_alloc(), av_frame_free);
    AVPacket packet;
    while (av_read_frame(format_ctx, &packet) == 0) {
      AVCodecContext *codec = format_ctx->streams[packet.stream_index]->codec;
      if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (!avcodec_is_open(codec)) {
          codec->thread_count = 1;
          codec->coding_hooks = &coding_hooks;
          if (avcodec_open2(codec, avcodec_find_decoder(codec->codec_id), nullptr) < 0) {
            throw std::runtime_error("Failed to open decoder for stream " + std::to_string(packet.stream_index));
          }
        }

        int got_frame = 0;
        if (avcodec_decode_video2(codec, frame.get(), &got_frame, &packet) < 0) {
          throw std::runtime_error("Failed to decode video frame.");
        }
      }
      av_packet_unref(&packet);
    }
  }

 private:
  // Hook stubs - wrap driver into opaque pointers.
  static int read_packet(void *opaque, uint8_t *buffer_out, int size) {
    decoder *self = static_cast<decoder*>(opaque);
    return self->driver->read_packet(buffer_out, size);
  }
  static void* init_cabac_decoder(void *opaque, CABACContext *ctx, const uint8_t *buf, int size) {
    decoder *self = static_cast<decoder*>(opaque);
    auto *cabac_decoder = new typename Driver::cabac_decoder(self->driver, ctx, buf, size);
    self->cabac_contexts[ctx].reset(cabac_decoder);
    return cabac_decoder;
  }

  Driver *driver;
  AVFormatContext *format_ctx;
  AVCodecCodingHooks coding_hooks = { this, { init_cabac_decoder } };
  std::map<CABACContext*, std::unique_ptr<typename Driver::cabac_decoder>> cabac_contexts;
};


class compressor {
 public:
  compressor(const std::string& input_filename, std::ostream& out_stream)
    : input_filename(input_filename), out_stream(out_stream) {
    if (av_file_map(input_filename.c_str(), &original_bytes, &original_size, 0, NULL) < 0) {
      throw std::invalid_argument("Failed to open file: " + input_filename);
    }
  }

  ~compressor() {
    av_file_unmap(original_bytes, original_size);
  }

  void run() {
    // Run through all the frames in the file, building the output using our hooks.
    decoder<compressor> d(this, input_filename);
    d.dump_stream_info();
    d.decode_video();

    // Flush the final block to the output and write to stdout.
    out.add_block()->set_literal(
        &original_bytes[prev_coded_block_end], original_size - prev_coded_block_end);
    out_stream << out.SerializeAsString();
  }

  int read_packet(uint8_t *buffer_out, int size) {
    size = std::min<int>(size, original_size - read_offset);
    memcpy(buffer_out, &original_bytes[read_offset], size);
    read_offset += size;
    return size;
  }

  class cabac_decoder {
   public:
    cabac_decoder(compressor *c, CABACContext *ctx, const uint8_t *buf, int size) {
      ::ff_reset_cabac_decoder(ctx, buf, size);
      out = c->find_next_coded_block_and_emit_literal(buf, size);
      if (out == nullptr) out = &out_to_ignore;
      out->set_cabac(buf, size);
    }

   private:
    Recoded::Block *out;
    Recoded::Block out_to_ignore;
  };

 private:
  Recoded::Block* find_next_coded_block_and_emit_literal(const uint8_t *buf, int size) {
    uint8_t *found = static_cast<uint8_t*>( memmem(
        &original_bytes[prev_coded_block_end], read_offset - prev_coded_block_end,
        buf, size) );
    if (found && size >= SURROGATE_MARKER_BYTES) {
      int gap = found - &original_bytes[prev_coded_block_end];
      out.add_block()->set_literal(&original_bytes[prev_coded_block_end], gap);
      prev_coded_block_end += gap + size;
      return out.add_block();  // Return a block for the recoder to fill.
    } else {
      // Can't recode this block, probably because it was NAL-escaped. Place
      // a skip marker in the block list.
      out.add_block()->set_skip_coded(size);
      return nullptr;  // Tell the recoder to ignore this block.
    }
  }

  std::string input_filename;
  std::ostream& out_stream;

  uint8_t *original_bytes = nullptr;
  size_t original_size = 0;
  int read_offset = 0;
  int prev_coded_block_end = 0;

  Recoded out;
};


class decompressor {
 public:
  decompressor(const std::string& input_filename, std::ostream& out_stream)
    : input_filename(input_filename), out_stream(out_stream) {
    uint8_t *bytes;
    size_t size;
    if (av_file_map(input_filename.c_str(), &bytes, &size, 0, NULL) < 0) {
      throw std::invalid_argument("Failed to open file: " + input_filename);
    }
    in.ParseFromArray(bytes, size);
  }
  decompressor(const std::string& input_filename, const std::string& in_bytes, std::ostream& out_stream)
    : input_filename(input_filename), out_stream(out_stream) {
    in.ParseFromString(in_bytes);
  }

  void run() {
    decoder<decompressor> d(this, input_filename);
    d.decode_video();
  }

  int read_packet(uint8_t *buffer_out, int size) {
    uint8_t *p = buffer_out;
    while (size > 0 && read_index < in.block_size()) {
      if (read_block.empty()) {
        const Recoded::Block& block = in.block(read_index);
        if (block.has_literal()) {
          read_block = block.literal();
        } else if (block.has_cabac()) {
          read_block += make_surrogate_block(read_index, block.cabac().size());
        } else if (block.has_skip_coded()) {
          surrogate_queue.push_back(std::make_pair("", read_index));
        } else {
          throw std::runtime_error("Unknown input block type");
        }
      }
      if (read_offset < read_block.size()) {
        int n = read_block.copy(reinterpret_cast<char*>(p), size, read_offset);
        read_offset += n;
        p += n;
        size -= n;
      }
      if (read_offset >= read_block.size()) {
        out_stream << in.block(read_index).literal();
        out_stream << in.block(read_index).cabac();

        read_block.clear();
        read_offset = 0;
        read_index++;
      }
    }
    return p - buffer_out;
  }

  class cabac_decoder {
   public:
    cabac_decoder(decompressor *d, CABACContext *ctx, const uint8_t *buf, int size) {
      const Recoded::Block& block = d->recognize_surrogate_block(buf, size);
      if (block.has_cabac()) {
        block.cabac().copy(reinterpret_cast<char*>(const_cast<uint8_t*>(buf)), size);
      }
      ::ff_reset_cabac_decoder(ctx, buf, size);
    }
  };

 private:
  std::string make_surrogate_block(int index, int size) {
    if (size < SURROGATE_MARKER_BYTES) {
      throw std::runtime_error("Invalid surrogate block size: " + std::to_string(size));
    }
    std::string surrogate = next_surrogate_marker();
    surrogate_queue.push_back(std::make_pair(surrogate, index));
    surrogate.resize(size, 'X');  // NAL-encoding-safe padding.
    return surrogate;
  }

  // Return a unique 8-byte string containing no zero bytes (NAL-encoding-safe).
  std::string next_surrogate_marker() {
    if (surrogate_marker.empty()) {
      surrogate_marker.resize(SURROGATE_MARKER_BYTES, '\x01');
    }
    for (int i = 0; i < surrogate_marker.size(); i++) {
      if (surrogate_marker[i] != '\xFF') {
        surrogate_marker[i]++;
        return surrogate_marker;
      } else {
        // Reset to 1 and carry.
        surrogate_marker[i] = '\x01';
      }
    }
    throw std::runtime_error("More than 255^8 (~2^64) surrogate blocks (very unlikely).");
  }
  
  const Recoded::Block& recognize_surrogate_block(const uint8_t* buf, int size) {
    if (surrogate_queue.empty()) {
      throw std::runtime_error("Coded block expected, but not recorded in the compressed data.");
    }
    const std::string& expected_surrogate_marker = surrogate_queue.front().first;
    int index = surrogate_queue.front().second;
    const Recoded::Block& block = in.block(index);
    surrogate_queue.pop_front();

    if (expected_surrogate_marker.empty()) {
      if (!block.has_skip_coded()) throw std::runtime_error("Corrupt surrogate_queue");
      if (block.skip_coded() != size) throw std::runtime_error("Invalid skip_coded block size");
      return block;
    }

    if (block.cabac().size() != size) {
      throw std::runtime_error("Invalid surrogate block size: expected " +
          std::to_string(block.cabac().size()) + ", got " + std::to_string(size));
    }
    std::string buf_header(reinterpret_cast<const char*>(buf), expected_surrogate_marker.size());
    if (expected_surrogate_marker != buf_header) {
      throw std::runtime_error("Invalid surrogate marker in coded block.");
    }
    return block;
  }

  std::string input_filename;
  std::ostream& out_stream;

  Recoded in;
  int read_index = 0, read_offset = 0;
  std::string read_block;

  // List of (surrogate marker, block index) pairs. Blocks with skip_coded have marker=="".
  std::list<std::pair<std::string, int>> surrogate_queue;
  // Marker for the last surrogate block.
  std::string surrogate_marker;
};


int roundtrip(const std::string& input_filename, std::ostream* out) {
  std::stringstream original, compressed, decompressed;
  original << std::ifstream(input_filename).rdbuf();
  compressor c(input_filename, compressed);
  c.run();
  decompressor d(input_filename, compressed.str(), decompressed);
  d.run();

  if (original.str() == decompressed.str()) {
    if (out) {
      (*out) << compressed.str();
    }
    double ratio = compressed.str().size() * 1.0 / original.str().size();

    Recoded compressed_proto;
    compressed_proto.ParseFromString(compressed.str());
    int proto_block_bytes = 0;
    for (const auto& block : compressed_proto.block()) {
      proto_block_bytes += block.literal().size() + block.cabac().size();
    }
    double proto_overhead = (compressed.str().size() - proto_block_bytes) * 1.0 / compressed.str().size();

    std::cout << "Compress-decompress roundtrip succeeded:" << std::endl;
    std::cout << " compression ratio: " << ratio*100. << "%" << std::endl;
    std::cout << " protobuf overhead: " << proto_overhead*100. << "%" << std::endl;
    return 0;
  } else {
    std::cerr << "Compress-decompress roundtrip failed." << std::endl;
    return 1;
  }
}


int
main(int argc, char **argv) {
  av_register_all();

  if (argc < 3 || argc > 4) {
    std::cerr << "Usage: " << argv[0] << " [compress|decompress|roundtrip] <input> [output]" << std::endl;
    return 1;
  }
  std::string command = argv[1];
  std::string input_filename = argv[2];
  std::ofstream out_file;
  if (argc > 3) {
    out_file.open(argv[3]);
  }

  try {
    if (command == "compress") {
      compressor c(input_filename, out_file.is_open() ? out_file : std::cout);
      c.run();
    } else if (command == "decompress") {
      decompressor d(input_filename, out_file.is_open() ? out_file : std::cout);
      d.run();
    } else if (command == "roundtrip") {
      return roundtrip(input_filename, out_file.is_open() ? &out_file : nullptr);
    } else {
      throw std::invalid_argument("Unknown command: " + command);
    }
  } catch (const std::exception& e) {
    std::cerr << "Exception (" << typeid(e).name() << "): " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
