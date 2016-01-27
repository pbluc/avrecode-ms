#include <iostream>
#include <typeinfo>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavutil/file.h"
}



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
decode_video(AVFormatContext* input_format_ctx) {
  if (avformat_find_stream_info(input_format_ctx, nullptr) < 0) {
    throw std::runtime_error("Invalid input stream information.\n");
  }
  av_dump_format(input_format_ctx, 0, input_format_ctx->filename, 0);

  for (int i = 0; i < input_format_ctx->nb_streams; i++) {
    AVCodecContext *codec = input_format_ctx->streams[i]->codec;
    if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (avcodec_open2(codec, avcodec_find_decoder(codec->codec_id), nullptr) < 0) {
        throw std::runtime_error("Failed to open decoder for stream " + std::to_string(i));
      }
    }
  }
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
      int got_frame = 0;
      if (avcodec_decode_video2(codec, frame.get(), &got_frame, &packet) < 0) {
        throw std::runtime_error("Failed to decode video frame.");
      }
    }
    av_packet_unref(&packet);
  }
}


struct buffer {
  uint8_t *bytes = nullptr;
  size_t size = 0;
  size_t offset = 0;
};


int
read_packet(void *opaque, uint8_t *buffer_out, int size) {
  struct buffer *file_buffer = (struct buffer*)opaque;
  size = FFMIN(size, file_buffer->size - file_buffer->offset);

  av_log(NULL, AV_LOG_INFO, "read_packet offset:%zu size:%d\n", file_buffer->offset, size);

  memcpy(buffer_out, &file_buffer->bytes[file_buffer->offset], size);
  file_buffer->offset += size;
  return size;
}


void
compress(const std::string& input_filename) {
  struct buffer file_buffer;
  if (av_file_map(input_filename.c_str(), &file_buffer.bytes, &file_buffer.size, 0, NULL) < 0) {
    throw std::invalid_argument("Failed to open file: " + input_filename);
  }
  defer<> unmap_file_buffer([=](){ av_file_unmap(file_buffer.bytes, file_buffer.size); });

  const size_t avio_ctx_buffer_size = 1024*1024;
  auto avio_ctx_buffer = av_unique_ptr(av_malloc(avio_ctx_buffer_size));

  auto format_ctx = av_unique_ptr(avformat_alloc_context(), avformat_close_input);
  format_ctx->pb = avio_alloc_context(
      (uint8_t*)avio_ctx_buffer.release(),  // input buffer
      avio_ctx_buffer_size,                 // input buffer size
      false,                                // stream is not writable
      &file_buffer,                         // first argument for read_packet()
      &read_packet,                         // read callback
      nullptr,                              // write_packet()
      nullptr);                             // seek()
  defer<> free_avio_ctx([&](){
    av_freep(&format_ctx->pb->buffer);  // May no longer be avio_ctx_buffer.
    av_freep(&format_ctx->pb);
  });

  AVFormatContext *input_format_ctx = format_ctx.get();
  if (avformat_open_input(&input_format_ctx, input_filename.c_str(), nullptr, nullptr) < 0) {
    throw std::invalid_argument("Failed to open file: " + input_filename);
  }

  decode_video(format_ctx.get());
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
      compress(input_filename);
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
