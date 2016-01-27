#include <fstream>
#include <iostream>
#include <sstream>
#include <typeinfo>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavcodec/coding_hooks.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavutil/file.h"
}

#include "recode.pb.h"



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


template <typename T>
struct hooks {
  static int read_packet(void *opaque, uint8_t *buffer_out, int size) {
    T* object = (T*)opaque;
    return object->read_packet(buffer_out, size);
  }
  static void init_cabac_decoder(void *opaque, CABACContext *c, const uint8_t *buf, int size) {
    T* object = (T*)opaque;
    object->init_cabac_decoder(c, buf, size);
  }
  static AVCodecCodingHooks coding_hooks(T* object) {
    return {
      object,
      init_cabac_decoder,
    };
  }
};


// Sets up a libavcodec decoder with I/O and decoding hooks.
template <typename Driver>
class decoder {
 public:
  decoder(Driver *driver, const std::string& input_filename)
    : driver(driver), coding_hooks(hooks<Driver>::coding_hooks(driver)) {
    const size_t avio_ctx_buffer_size = 1024*1024;
    uint8_t *avio_ctx_buffer = (uint8_t*)av_malloc(avio_ctx_buffer_size);

    format_ctx = avformat_alloc_context();
    if (avio_ctx_buffer == nullptr || format_ctx == nullptr) throw std::bad_alloc();
    format_ctx->pb = avio_alloc_context(
        avio_ctx_buffer,                      // input buffer
        avio_ctx_buffer_size,                 // input buffer size
        false,                                // stream is not writable
        driver,                               // first argument for read_packet()
        &hooks<Driver>::read_packet,          // read callback
        nullptr,                              // write_packet()
        nullptr);                             // seek()

    if (avformat_open_input(&format_ctx, input_filename.c_str(), nullptr, nullptr) < 0) {
      throw std::invalid_argument("Failed to open file: " + input_filename);
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
  Driver *driver;
  AVCodecCodingHooks coding_hooks;
  AVFormatContext *format_ctx;
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
    //std::cerr << "final gap: " << original_size - last_bit_block_end << std::endl;
    out.add_literal_blocks(&original_bytes[last_bit_block_end], original_size - last_bit_block_end);
    out_stream << out.SerializeAsString();
  }

  int read_packet(uint8_t *buffer_out, int size) {
    size = std::min<int>(size, original_size - read_offset);

    //av_log(NULL, AV_LOG_INFO, "read_packet offset:%d size:%d\n", read_offset, size);

    memcpy(buffer_out, &original_bytes[read_offset], size);
    read_offset += size;
    return size;
  }

  void init_cabac_decoder(CABACContext *c, const uint8_t *buf, int size) {
    uint8_t *found = (uint8_t*) memmem(
        &original_bytes[last_bit_block_end], read_offset-last_bit_block_end,
        buf, size);
    if (found != nullptr) {
      int gap = found - &original_bytes[last_bit_block_end];
      out.add_literal_blocks(&original_bytes[last_bit_block_end], gap);
      out.add_cabac_blocks(&original_bytes[last_bit_block_end + gap], size);
      last_bit_block_end += gap + size;
      //std::cerr << "compressing bit block after gap: " << gap << " size: " << size << std::endl;
    } else {
      //std::cerr << "cabac block with escapes, size: " << size << std::endl;
    }
  }

 private:
  std::string input_filename;
  std::ostream& out_stream;

  uint8_t *original_bytes = nullptr;
  size_t original_size = 0;
  int read_offset = 0;

  int last_bit_block_end = 0;

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
    while (size > 0) {
      if (read_index >= in.literal_blocks_size()) {
        break;
      }
      const std::string& literal = in.literal_blocks(read_index);
      if (read_offset_literal < literal.size()) {
        int n = literal.copy((char*)p, size, read_offset_literal);
        read_offset_literal += n;
        p += n;
        size -= n;
      } else if (read_index < in.cabac_blocks_size() &&
          read_offset_cabac < in.cabac_blocks(read_index).size()) {
        const std::string& cabac = in.cabac_blocks(read_index);
        int n = cabac.copy((char*)p, size, read_offset_cabac);
        read_offset_cabac += n;
        p += n;
        size -= n;
      } else {
        out_stream << in.literal_blocks(read_index);
        if (read_index < in.cabac_blocks_size()) {
          out_stream << in.cabac_blocks(read_index);
        }
        read_index++;
        read_offset_literal = read_offset_cabac = 0;
      }
    }
    return p - buffer_out;
  }

  void init_cabac_decoder(CABACContext *c, const uint8_t *buf, int size) {
//    int gap = 0;
//    std::cerr << "compressing bit block after gap: " << gap << " size: " << size << std::endl;
  }

 private:
  std::string input_filename;
  std::ostream& out_stream;

  Recoded in;
  int read_index = 0, read_offset_literal = 0, read_offset_cabac = 0;
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
    std::cout << "Compress-decompress roundtrip succeeded, ratio = " << ratio*100. << "%." << std::endl;
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
