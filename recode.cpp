#include <iostream>
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
  explicit defer(const T& to_defer_in) : to_defer(to_defer_in) {}
  defer(const defer&) = delete;
  ~defer() { to_defer(); }
};


void
decode_video(AVFormatContext *input_format_ctx, AVCodecCodingHooks *hooks) {
  defer<> close_streams([&](){
    for (int i = 0; i < input_format_ctx->nb_streams; i++) {
      avcodec_close(input_format_ctx->streams[i]->codec);
    }
  });

  auto frame = av_unique_ptr(av_frame_alloc(), av_frame_free);
  AVPacket packet;
  while (av_read_frame(input_format_ctx, &packet) == 0) {
    AVCodecContext *codec = input_format_ctx->streams[packet.stream_index]->codec;
    if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (!avcodec_is_open(codec)) {
        codec->thread_count = 1;
        codec->coding_hooks = hooks;
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


class compressor {
 public:
  compressor(const std::string& input_filename_in) : input_filename(input_filename_in) {
    if (av_file_map(input_filename.c_str(), &input_bytes, &input_size, 0, NULL) < 0) {
      throw std::invalid_argument("Failed to open file: " + input_filename);
    }
  }

  ~compressor() {
    av_file_unmap(input_bytes, input_size);
  }

  void run();

  int read_packet(uint8_t *buffer_out, int size) {
    size = std::min<int>(size, input_size - input_offset);

    av_log(NULL, AV_LOG_INFO, "read_packet offset:%d size:%d\n", input_offset, size);

    memcpy(buffer_out, &input_bytes[input_offset], size);
    input_offset += size;
    return size;
  }

  void init_cabac_decoder(CABACContext *c, const uint8_t *buf, int size) {
    uint8_t *found = (uint8_t*) memmem(
        &input_bytes[last_bit_block_end], input_offset-last_bit_block_end,
        buf, size);
    if (found != nullptr) {
      int gap = found - &input_bytes[last_bit_block_end];
      out.add_raw_blocks(&input_bytes[last_bit_block_end], gap);
      out.add_cabac_blocks(&input_bytes[last_bit_block_end + gap], size);
      last_bit_block_end += gap + size;
      std::cerr << "compressing bit block after gap: " << gap << " size: " << size << std::endl;
    }
  }

 private:
  std::string input_filename;
  uint8_t *input_bytes = nullptr;
  size_t input_size = 0;
  int input_offset = 0;

  int last_bit_block_end = 0;

  Recoded out;
};


void
compressor::run() {
  const size_t avio_ctx_buffer_size = 1024*1024;
  auto avio_ctx_buffer = av_unique_ptr(av_malloc(avio_ctx_buffer_size));

  // Set up the IO context to read from our read_packet hook.
  auto format_ctx = av_unique_ptr(avformat_alloc_context(), avformat_close_input);
  format_ctx->pb = avio_alloc_context(
      (uint8_t*)avio_ctx_buffer.release(),  // input buffer
      avio_ctx_buffer_size,                 // input buffer size
      false,                                // stream is not writable
      this,                                 // first argument for read_packet()
      &hooks<compressor>::read_packet,      // read callback
      nullptr,                              // write_packet()
      nullptr);                             // seek()
  defer<> free_avio_ctx([&](){
    av_freep(&format_ctx->pb->buffer);  // May no longer be avio_ctx_buffer.
    av_freep(&format_ctx->pb);
  });

  // Open the input file (reading from the already in-memory blocks) and dump stream info.
  AVFormatContext *input_format_ctx = format_ctx.get();
  if (avformat_open_input(&input_format_ctx, input_filename.c_str(), nullptr, nullptr) < 0) {
    throw std::invalid_argument("Failed to open file: " + input_filename);
  }

  if (avformat_find_stream_info(input_format_ctx, nullptr) < 0) {
    throw std::runtime_error("Invalid input stream information.\n");
  }
  av_dump_format(input_format_ctx, 0, input_format_ctx->filename, 0);

  // Run through all the frames in the file, building the output using our hooks.
  AVCodecCodingHooks coding_hooks = hooks<compressor>::coding_hooks(this);
  decode_video(format_ctx.get(), &coding_hooks);

  // Flush the final block to the output and write to stdout.
  std::cerr << "final gap: " << input_size - last_bit_block_end << std::endl;
  out.add_raw_blocks(&input_bytes[last_bit_block_end], input_size - last_bit_block_end);

  std::string out_string;
  out.SerializeToString(&out_string);
  std::cout << out_string;
}


void
decompress(const std::string& input_filename) {
}


int
main(int argc, char **argv) {
  av_register_all();

  if (argc != 3) {
    std::cerr << "Usage: " << argv[0] << " [compress|decompress] <file>" << std::endl;
    return 1;
  }
  std::string command = argv[1];
  std::string input_filename = argv[2];

  try {
    if (command == "compress") {
      compressor(input_filename).run();
    } else if (command == "decompress") {
      decompress(input_filename);
    } else {
      throw std::invalid_argument("Unknown command: " + command);
    }
  } catch (const std::exception& e) {
    std::cerr << "Exception (" << typeid(e).name() << "): " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
