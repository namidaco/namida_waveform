#include "nw_decoder.h"

#include <string.h>

#include "namida_waveform.h"

/// Added to the decoder context's log level so a damaged stream cannot spam
/// thousands of lines per track. Only the decoder supports this; the process
/// wide level is left alone because the host app uses it too.
#define NW_LOG_OFFSET 100

int32_t nw_decoder_open(NWDecoder* decoder, const char* path) {
  memset(decoder, 0, sizeof(*decoder));
  decoder->stream_index = -1;
  if (path == NULL) return NW_ERR_OPEN_INPUT;

  // On failure avformat_open_input already freed the context.
  if (avformat_open_input(&decoder->fmt_ctx, path, NULL, NULL) < 0) return NW_ERR_OPEN_INPUT;
  if (avformat_find_stream_info(decoder->fmt_ctx, NULL) < 0) return NW_ERR_STREAM_INFO;

  AVFormatContext* fmt_ctx = decoder->fmt_ctx;
  const int stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (stream_index < 0) return NW_ERR_NO_AUDIO_STREAM;
  decoder->stream_index = stream_index;
  AVStream* stream = fmt_ctx->streams[stream_index];

  // Demuxers that can skip a discarded stream's data (mov, matroska, mpegts)
  // then never read the video out of a video file.
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    if ((int)i != stream_index) fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
  }

  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (codec == NULL) return NW_ERR_NO_DECODER;

  decoder->dec_ctx = avcodec_alloc_context3(codec);
  if (decoder->dec_ctx == NULL) return NW_ERR_ALLOC;
  AVCodecContext* dec_ctx = decoder->dec_ctx;
  if (avcodec_parameters_to_context(dec_ctx, stream->codecpar) < 0) return NW_ERR_DECODER_OPEN;

  dec_ctx->log_level_offset = NW_LOG_OFFSET;

  // Let libavcodec pick a thread count; decoders without threading support
  // silently ignore it.
  dec_ctx->thread_count = 0;
  dec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

  if (avcodec_open2(dec_ctx, codec, NULL) < 0) return NW_ERR_DECODER_OPEN;

  if (fmt_ctx->duration > 0 && fmt_ctx->duration != AV_NOPTS_VALUE) {
    decoder->duration_ms = fmt_ctx->duration / (AV_TIME_BASE / 1000);
  } else if (stream->duration > 0 && stream->duration != AV_NOPTS_VALUE) {
    decoder->duration_ms = (int64_t)(stream->duration * av_q2d(stream->time_base) * 1000.0);
  }

  decoder->frame = av_frame_alloc();
  decoder->packet = av_packet_alloc();
  if (decoder->frame == NULL || decoder->packet == NULL) return NW_ERR_ALLOC;
  return NW_OK;
}

int nw_decoder_next(NWDecoder* decoder) {
  for (;;) {
    if (decoder->receiving) {
      if (avcodec_receive_frame(decoder->dec_ctx, decoder->frame) >= 0) {
        decoder->received = 1;
        return 1;
      }
      decoder->receiving = 0;
      // Once flushed, the decoder only stops with nothing left to receive.
      if (decoder->draining) return 0;
      // A packet the decoder keeps refusing without producing anything would
      // otherwise be resent forever.
      if (decoder->pending && !decoder->received) {
        av_packet_unref(decoder->packet);
        decoder->pending = 0;
      }
    }

    if (!decoder->pending) {
      if (av_read_frame(decoder->fmt_ctx, decoder->packet) < 0) {
        decoder->draining = 1;
        avcodec_send_packet(decoder->dec_ctx, NULL);
        decoder->received = 0;
        decoder->receiving = 1;
        continue;
      }
      if (decoder->packet->stream_index != decoder->stream_index) {
        av_packet_unref(decoder->packet);
        continue;
      }
    }

    const int sent = avcodec_send_packet(decoder->dec_ctx, decoder->packet);
    decoder->pending = sent == AVERROR(EAGAIN);
    if (!decoder->pending) {
      av_packet_unref(decoder->packet);
      // A corrupt packet should not throw away everything decoded so far.
      if (sent < 0) continue;
    }
    decoder->received = 0;
    decoder->receiving = 1;
  }
}

void nw_decoder_close(NWDecoder* decoder) {
  if (decoder->packet != NULL) av_packet_free(&decoder->packet);
  if (decoder->frame != NULL) av_frame_free(&decoder->frame);
  if (decoder->dec_ctx != NULL) avcodec_free_context(&decoder->dec_ctx);
  if (decoder->fmt_ctx != NULL) avformat_close_input(&decoder->fmt_ctx);
}

/// Averages every channel of `frame` into mono samples at `dst`.
#define NW_DOWNMIX(TYPE, BIAS, SCALE)                                             do {                                                                              const float gain = (float)((SCALE) / channels);                                 if (planar) {                                                                     const TYPE* p = (const TYPE*)frame->extended_data[0];                           for (int i = 0; i < n; i++) dst[i] = (float)(p[i] - (BIAS));                    for (int ch = 1; ch < channels; ch++) {                                           p = (const TYPE*)frame->extended_data[ch];                                      for (int i = 0; i < n; i++) dst[i] += (float)(p[i] - (BIAS));                 }                                                                               for (int i = 0; i < n; i++) dst[i] *= gain;                                   } else {                                                                          const TYPE* p = (const TYPE*)frame->extended_data[0];                           for (int i = 0; i < n; i++) {                                                     float mixed = 0.0f;                                                             for (int ch = 0; ch < channels; ch++) mixed += (float)(p[ch] - (BIAS));         dst[i] = mixed * gain;                                                          p += channels;                                                                }                                                                             }                                                                             } while (0)

int32_t nw_decoder_downmix(const AVFrame* frame, float* dst) {
  const enum AVSampleFormat format = (enum AVSampleFormat)frame->format;
  const int planar = av_sample_fmt_is_planar(format);
  const int n = frame->nb_samples;
  const int channels = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 1;

  switch (format) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_U8P:
      NW_DOWNMIX(uint8_t, 128, 1.0 / 128.0);
      break;
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
      NW_DOWNMIX(int16_t, 0, 1.0 / 32768.0);
      break;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
      NW_DOWNMIX(int32_t, 0, 1.0 / 2147483648.0);
      break;
    case AV_SAMPLE_FMT_S64:
    case AV_SAMPLE_FMT_S64P:
      NW_DOWNMIX(int64_t, 0, 1.0 / 9223372036854775808.0);
      break;
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:
      NW_DOWNMIX(float, 0, 1.0);
      break;
    case AV_SAMPLE_FMT_DBL:
    case AV_SAMPLE_FMT_DBLP:
      NW_DOWNMIX(double, 0, 1.0);
      break;
    default:
      return NW_ERR_UNSUPPORTED_FORMAT;
  }
  return NW_OK;
}
