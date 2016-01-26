extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
}


int
main(int argc, char **argv)
{
  av_register_all();

  AVFormatContext *input_format_ctx;
  if (avformat_open_input(&input_format_ctx, argv[1], nullptr, nullptr) < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot open input file.\n");
    return 1;
  }
  if (avformat_find_stream_info(input_format_ctx, nullptr) < 0) {
    av_log(NULL, AV_LOG_ERROR, "Invalid input stream information.\n");
    return 1;
  }
  av_dump_format(input_format_ctx, 0, argv[1], 0);
  for (int i = 0; i < input_format_ctx->nb_streams; i++) {
    AVCodecContext *codec = input_format_ctx->streams[i]->codec;
    if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (avcodec_open2(codec, avcodec_find_decoder(codec->codec_id), nullptr) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
        return 1;
      }
    }
  }

  AVPacket packet;
  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    av_log(NULL, AV_LOG_ERROR, "Memory allocation error.\n");
    return 1;
  }
  while (1) {
    if (av_read_frame(input_format_ctx, &packet) < 0)
      break;

    AVCodecContext *codec = input_format_ctx->streams[packet.stream_index]->codec;
    if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
      int got_frame = 0;
      if (avcodec_decode_video2(codec, frame, &got_frame, &packet) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Decoding failed.\n");
        return 1;
      }
    }
    av_packet_unref(&packet);
  }

  av_frame_free(&frame);
  for (int i = 0; i < input_format_ctx->nb_streams; i++) {
    avcodec_close(input_format_ctx->streams[i]->codec);
  }
  avformat_close_input(&input_format_ctx);
  return 0;
}
