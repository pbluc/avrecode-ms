#include <fstream>
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
  compressor(const std::string& input_filename_in, std::ostream& out_stream_in)
    : input_filename(input_filename_in), out_stream(out_stream_in) {
    if (av_file_map(input_filename.c_str(), &original_bytes, &original_size, 0, NULL) < 0) {
      throw std::invalid_argument("Failed to open file: " + input_filename);
    }
  }

  ~compressor() {
    av_file_unmap(original_bytes, original_size);
  }

  void run();

  int read_packet(uint8_t *buffer_out, int size) {
    size = std::min<int>(size, original_size - read_offset);

    av_log(NULL, AV_LOG_INFO, "read_packet offset:%d size:%d\n", read_offset, size);

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
      out.add_raw_blocks(&original_bytes[last_bit_block_end], gap);
      out.add_cabac_blocks(&original_bytes[last_bit_block_end + gap], size);
      last_bit_block_end += gap + size;
      std::cerr << "compressing bit block after gap: " << gap << " size: " << size << std::endl;
    } else {
      std::cerr << "cabac block with escapes, size: " << size << std::endl;
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
  std::cerr << "final gap: " << original_size - last_bit_block_end << std::endl;
  out.add_raw_blocks(&original_bytes[last_bit_block_end], original_size - last_bit_block_end);
  out_stream << out.SerializeAsString();
}


class decompressor {
 public:
  decompressor(const std::string& input_filename_in, std::ostream& out_stream_in)
    : input_filename(input_filename_in), out_stream(out_stream_in) {
    uint8_t *bytes;
    size_t size;
    if (av_file_map(input_filename.c_str(), &bytes, &size, 0, NULL) < 0) {
      throw std::invalid_argument("Failed to open file: " + input_filename);
    }
    in.ParseFromArray(bytes, size);
  }

  void run();

  int read_packet(uint8_t *buffer_out, int size) {
    return 0;
  }

  void init_cabac_decoder(CABACContext *c, const uint8_t *buf, int size) {
    int gap = 0;
    std::cerr << "compressing bit block after gap: " << gap << " size: " << size << std::endl;
  }

 private:
  std::string input_filename;
  std::ostream& out_stream;

  Recoded in;
};


void
decompressor::run() {
  for (int i = 0; i < in.raw_blocks_size()-1; i++) {
    out_stream << in.raw_blocks(i);
    out_stream << in.cabac_blocks(i);
  }
  out_stream << in.raw_blocks(in.raw_blocks_size()-1);
#if 0
  const size_t avio_ctx_buffer_size = 1024*1024;
  auto avio_ctx_buffer = av_unique_ptr(av_malloc(avio_ctx_buffer_size));

  // Set up the IO context to read from our read_packet hook.
  auto format_ctx = av_unique_ptr(avformat_alloc_context(), avformat_close_input);
  format_ctx->pb = avio_alloc_context(
      (uint8_t*)avio_ctx_buffer.release(),  // input buffer
      avio_ctx_buffer_size,                 // input buffer size
      false,                                // stream is not writable
      this,                                 // first argument for read_packet()
      &hooks<decompressor>::read_packet,    // read callback
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

  // Run through all the frames in the file, building the output using our hooks.
  AVCodecCodingHooks coding_hooks = hooks<decompressor>::coding_hooks(this);
  decode_video(format_ctx.get(), &coding_hooks);

  // Flush the final block to the output and write to stdout.
//  std::cerr << "final gap: " << original_size - last_bit_block_end << std::endl;
//  out.add_raw_blocks(&original_bytes[last_bit_block_end], original_size - last_bit_block_end);
#endif
}


int
main(int argc, char **argv) {
  av_register_all();

  if (argc < 3 || argc > 4) {
    std::cerr << "Usage: " << argv[0] << " [compress|decompress] <input> [output]" << std::endl;
    return 1;
  }
  std::string command = argv[1];
  std::string input_filename = argv[2];
  std::ostream *out = &std::cout;
  std::ofstream out_file;
  if (argc > 3) {
    out_file.open(argv[3]);
    out = &out_file;
  }

  try {
    if (command == "compress") {
      compressor c(input_filename, *out);
      c.run();
    } else if (command == "decompress") {
      decompressor d(input_filename, *out);
      d.run();
    } else {
      throw std::invalid_argument("Unknown command: " + command);
    }
  } catch (const std::exception& e) {
    std::cerr << "Exception (" << typeid(e).name() << "): " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
