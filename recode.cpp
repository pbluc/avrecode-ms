#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <sstream>
#include <typeinfo>
#include <vector>

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
  struct cabac {
    static void* init_decoder(void *opaque, CABACContext *ctx, const uint8_t *buf, int size) {
      decoder *self = static_cast<decoder*>(opaque);
      auto *cabac_decoder = new typename Driver::cabac_decoder(self->driver, ctx, buf, size);
      self->cabac_contexts[ctx].reset(cabac_decoder);
      return cabac_decoder;
    }
    static int get(void *opaque, uint8_t *state) {
      auto *self = static_cast<typename Driver::cabac_decoder*>(opaque);
      return self->get(state);
    }
    static int get_bypass(void *opaque) {
      auto *self = static_cast<typename Driver::cabac_decoder*>(opaque);
      return self->get_bypass();
    }
    static int get_bypass_sign(void *opaque, int val) {
      auto *self = static_cast<typename Driver::cabac_decoder*>(opaque);
      return self->get_bypass_sign(val);
    }
    static int get_terminate(void *opaque) {
      auto *self = static_cast<typename Driver::cabac_decoder*>(opaque);
      return self->get_terminate();
    }
    static const uint8_t* skip_bytes(void *opaque, int n) {
      throw std::runtime_error("Not implemented: CABAC decoder doesn't use skip_bytes.");
    }
  };

  Driver *driver;
  AVFormatContext *format_ctx;
  AVCodecCodingHooks coding_hooks = { this, {
      cabac::init_decoder,
      cabac::get,
      cabac::get_bypass,
      cabac::get_bypass_sign,
      cabac::get_terminate,
      cabac::skip_bytes,
    },
  };
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
    cabac_decoder(compressor *c, CABACContext *ctx_in, const uint8_t *buf, int size) {
      out = c->find_next_coded_block_and_emit_literal(buf, size);
      if (out == nullptr) out = &out_to_ignore;
      ctx = *ctx_in;
      ctx.coding_hooks = nullptr;
      ctx.coding_hooks_opaque = nullptr;
      ::ff_reset_cabac_decoder(&ctx, buf, size);

      out->set_cabac(buf, size);
    }

    int get(uint8_t *state) {
      return ::ff_get_cabac(&ctx, state);
    }

    int get_bypass() {
      return ::ff_get_cabac_bypass(&ctx);
    }

    int get_bypass_sign(int val) {
      return ::ff_get_cabac_bypass_sign(&ctx, val);
    }

    int get_terminate() {
      return ::ff_get_cabac_terminate(&ctx);
    }

   private:
    Recoded::Block *out;
    Recoded::Block out_to_ignore;
    CABACContext ctx;
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
  // Used to track the decoding state of each block.
  struct block_state {
    bool coded = false;
    std::string surrogate_marker;
    std::string out_bytes;
    bool done = false;
  };

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
    blocks.clear();
    blocks.resize(in.block_size());

    decoder<decompressor> d(this, input_filename);
    d.decode_video();

    for (const auto& block : blocks) {
      if (!block.done) throw std::runtime_error("Not all blocks were decoded.");
      out_stream << block.out_bytes;
    }
  }

  int read_packet(uint8_t *buffer_out, int size) {
    uint8_t *p = buffer_out;
    while (size > 0 && read_index < in.block_size()) {
      if (read_block.empty()) {
        const Recoded::Block& block = in.block(read_index);
        if (int(block.has_literal()) + int(block.has_cabac()) + int(block.has_skip_coded()) != 1) {
          throw std::runtime_error("Invalid input block: must have exactly one type");
        }
        if (block.has_literal()) {
          // This block is passed through without any re-coding.
          blocks[read_index].out_bytes = block.literal();
          blocks[read_index].done = true;
          read_block = block.literal();
        } else if (block.has_cabac()) {
          // Re-coded CABAC coded block. out_bytes will be filled by cabac_decoder.
          blocks[read_index].coded = true;
          blocks[read_index].surrogate_marker = next_surrogate_marker();
          blocks[read_index].done = false;
          read_block = make_surrogate_block(blocks[read_index].surrogate_marker, block.cabac().size());
        } else if (block.has_skip_coded()) {
          // Non-re-coded CABAC coded block. The bytes of this block are
          // emitted in a literal block following this one. This block is
          // a flag to expect a cabac_decoder without a surrogate marker.
          blocks[read_index].coded = true;
          blocks[read_index].done = true;
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
        read_block.clear();
        read_offset = 0;
        read_index++;
      }
    }
    return p - buffer_out;
  }

  class cabac_decoder {
   public:
    cabac_decoder(decompressor *d, CABACContext *ctx_in, const uint8_t *buf, int size) {
      int index = d->recognize_coded_block(buf, size);
      block = &d->in.block(index);
      if (block->has_cabac()) {
        ctx = *ctx_in;
        ctx.coding_hooks = nullptr;
        ctx.coding_hooks_opaque = nullptr;

        block->cabac().copy(reinterpret_cast<char*>(const_cast<uint8_t*>(buf)), size);
        ::ff_reset_cabac_decoder(&ctx, buf, size);

        out = &d->blocks[index];
      } else if (block->has_skip_coded()) {
        // We're skipping this block, so disable calls to our hooks.
        ctx_in->coding_hooks = nullptr;
        ctx_in->coding_hooks_opaque = nullptr;
        ::ff_reset_cabac_decoder(ctx_in, buf, size);
      } else {
        throw std::runtime_error("Expected CABAC block.");
      }
    }

    int get(uint8_t *state) {
      return ::ff_get_cabac(&ctx, state);
    }

    int get_bypass() {
      return ::ff_get_cabac_bypass(&ctx);
    }

    int get_bypass_sign(int val) {
      return ::ff_get_cabac_bypass_sign(&ctx, val);
    }

    int get_terminate() {
      int n = ::ff_get_cabac_terminate(&ctx);
      if (n) {
        out->done = true;
        out->out_bytes = block->cabac();
      }
      return n;
    }

   private:
    const Recoded::Block *block;
    block_state *out = nullptr;
    CABACContext ctx;
  };

 private:
  // Return a unique 8-byte string containing no zero bytes (NAL-encoding-safe).
  std::string next_surrogate_marker() {
    uint64_t n = surrogate_marker_sequence_number++;
    std::string surrogate_marker(SURROGATE_MARKER_BYTES, '\x01');
    for (int i = 0; i < surrogate_marker.size(); i++) {
      surrogate_marker[i] = (n % 255) + 1;
      n /= 255;
    }
    return surrogate_marker;
  }
  
  std::string make_surrogate_block(const std::string& surrogate_marker, int size) {
    if (size < surrogate_marker.size()) {
      throw std::runtime_error("Invalid coded block size for surrogate: " + std::to_string(size));
    }
    std::string surrogate_block = surrogate_marker;
    surrogate_block.resize(size, 'X');  // NAL-encoding-safe padding.
    return surrogate_block;
  }

  int recognize_coded_block(const uint8_t* buf, int size) {
    while (!blocks[next_coded_block].coded) {
      if (next_coded_block >= read_index) {
        throw std::runtime_error("Coded block expected, but not recorded in the compressed data.");
      }
      next_coded_block++;
    }
    int index = next_coded_block++;
    // Validate the decoder init call against the coded block's size and surrogate marker.
    const Recoded::Block& block = in.block(index);
    if (block.has_cabac()) {
      if (block.cabac().size() != size) {
        throw std::runtime_error("Invalid surrogate block size.");
      }
      std::string buf_header(reinterpret_cast<const char*>(buf),
          blocks[index].surrogate_marker.size());
      if (blocks[index].surrogate_marker != buf_header) {
        throw std::runtime_error("Invalid surrogate marker in coded block.");
      }
    } else if (block.has_skip_coded()) {
      if (block.skip_coded() != size) {
        throw std::runtime_error("Invalid skip_coded block size.");
      }
    } else {
      throw std::runtime_error("Internal error: expected coded block.");
    }
    return index;
  }

  std::string input_filename;
  std::ostream& out_stream;

  Recoded in;
  int read_index = 0, read_offset = 0;
  std::string read_block;

  std::vector<block_state> blocks;

  // Counter used to generate surrogate markers for coded blocks.
  uint64_t surrogate_marker_sequence_number = 1;
  // Head of the coded block queue - blocks that have been produced by
  // read_packet but not yet decoded. Tail of the queue is read_index.
  int next_coded_block = 0;

  // List of (surrogate marker, block index) pairs. Blocks with skip_coded have marker=="".
  std::list<std::pair<std::string, int>> surrogate_queue;
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
