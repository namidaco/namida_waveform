#include "namida_waveform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

#define NW_VERSION 1

#define NW_MIN_SPS 1
#define NW_MAX_SPS 1000
#define NW_DEFAULT_SPS 100

/// Added to the decoder context's log level so a damaged stream cannot spam
/// thousands of lines per track. Only the decoder supports this; the process
/// wide level is left alone because the host app uses it too.
#define NW_LOG_OFFSET 100

/// Sum the squares of `take` sample positions starting at `pos`, across every
/// channel. `TYPE` is the storage type of the decoder output and `EXPR` maps one
/// stored value to the `-1..1` range.
///
/// Planar and packed are separate branches so the inner loop stays a flat walk
/// over contiguous memory in both cases.
#define NW_ACCUMULATE(TYPE, EXPR)                                            \
  do {                                                                       \
    if (planar) {                                                            \
      for (int ch = 0; ch < channels; ch++) {                                \
        const TYPE* p = ((const TYPE*)frame->extended_data[ch]) + pos;       \
        for (int i = 0; i < take; i++) {                                     \
          const double v = (EXPR);                                           \
          sum += v * v;                                                      \
        }                                                                    \
      }                                                                      \
    } else {                                                                 \
      const TYPE* p =                                                        \
          ((const TYPE*)frame->extended_data[0]) + (size_t)pos * channels;   \
      const int n = take * channels;                                         \
      for (int i = 0; i < n; i++) {                                          \
        const double v = (EXPR);                                             \
        sum += v * v;                                                        \
      }                                                                      \
    }                                                                        \
  } while (0)

typedef struct {
  float* values;
  int32_t count;
  int32_t capacity;
} NWBuffer;

static int nw_buffer_reserve(NWBuffer* buffer, int32_t needed) {
  if (needed <= buffer->capacity) return 1;
  int32_t capacity = buffer->capacity > 0 ? buffer->capacity : 1024;
  while (capacity < needed) {
    if (capacity > (INT32_MAX / 2)) {
      capacity = needed;
      break;
    }
    capacity *= 2;
  }
  float* values = (float*)realloc(buffer->values, (size_t)capacity * sizeof(float));
  if (values == NULL) return 0;
  buffer->values = values;
  buffer->capacity = capacity;
  return 1;
}

static int nw_buffer_push(NWBuffer* buffer, float value) {
  if (!nw_buffer_reserve(buffer, buffer->count + 1)) return 0;
  buffer->values[buffer->count++] = value;
  return 1;
}

static NWResult* nw_result_new(void) {
  NWResult* result = (NWResult*)calloc(1, sizeof(NWResult));
  return result;
}

static NWResult* nw_fail(NWResult* result, int32_t error) {
  if (result != NULL) result->error = error;
  return result;
}

NW_EXPORT int32_t nw_version(void) { return NW_VERSION; }

NW_EXPORT void nw_result_free(NWResult* result) {
  if (result == NULL) return;
  free(result->values);
  free(result);
}

NW_EXPORT NWResult* nw_extract(const char* path, int32_t samples_per_second) {
  NWResult* result = nw_result_new();
  if (result == NULL) return NULL;
  if (path == NULL) return nw_fail(result, NW_ERR_OPEN_INPUT);

  if (samples_per_second <= 0) samples_per_second = NW_DEFAULT_SPS;
  if (samples_per_second < NW_MIN_SPS) samples_per_second = NW_MIN_SPS;
  if (samples_per_second > NW_MAX_SPS) samples_per_second = NW_MAX_SPS;

  AVFormatContext* fmt_ctx = NULL;
  AVCodecContext* dec_ctx = NULL;
  AVFrame* frame = NULL;
  AVPacket* packet = NULL;
  NWBuffer buffer = {NULL, 0, 0};

  double sum = 0.0;
  int64_t bucket_filled = 0;
  int64_t bucket_samples = 0;
  int32_t error = NW_OK;

  if (avformat_open_input(&fmt_ctx, path, NULL, NULL) < 0) {
    // On failure avformat_open_input already freed the context.
    return nw_fail(result, NW_ERR_OPEN_INPUT);
  }
  if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
    error = NW_ERR_STREAM_INFO;
    goto cleanup;
  }

  const int stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (stream_index < 0) {
    error = NW_ERR_NO_AUDIO_STREAM;
    goto cleanup;
  }
  AVStream* stream = fmt_ctx->streams[stream_index];

  const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  if (decoder == NULL) {
    error = NW_ERR_NO_DECODER;
    goto cleanup;
  }

  dec_ctx = avcodec_alloc_context3(decoder);
  if (dec_ctx == NULL) {
    error = NW_ERR_ALLOC;
    goto cleanup;
  }
  if (avcodec_parameters_to_context(dec_ctx, stream->codecpar) < 0) {
    error = NW_ERR_DECODER_OPEN;
    goto cleanup;
  }

  dec_ctx->log_level_offset = NW_LOG_OFFSET;

  // Let libavcodec pick a thread count; decoders without threading support
  // silently ignore it.
  dec_ctx->thread_count = 0;
  dec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

  if (avcodec_open2(dec_ctx, decoder, NULL) < 0) {
    error = NW_ERR_DECODER_OPEN;
    goto cleanup;
  }

  if (fmt_ctx->duration > 0 && fmt_ctx->duration != AV_NOPTS_VALUE) {
    result->duration_ms = fmt_ctx->duration / (AV_TIME_BASE / 1000);
  } else if (stream->duration > 0 && stream->duration != AV_NOPTS_VALUE) {
    result->duration_ms = (int64_t)(stream->duration * av_q2d(stream->time_base) * 1000.0);
  }

  const int sample_rate = dec_ctx->sample_rate > 0 ? dec_ctx->sample_rate : 44100;
  result->sample_rate = sample_rate;
  result->channels = dec_ctx->ch_layout.nb_channels;

  bucket_samples = sample_rate / samples_per_second;
  if (bucket_samples < 1) bucket_samples = 1;

  if (result->duration_ms > 0) {
    const int64_t estimate = (result->duration_ms * samples_per_second) / 1000 + 64;
    if (estimate > 0 && estimate < INT32_MAX && !nw_buffer_reserve(&buffer, (int32_t)estimate)) {
      error = NW_ERR_ALLOC;
      goto cleanup;
    }
  }

  frame = av_frame_alloc();
  packet = av_packet_alloc();
  if (frame == NULL || packet == NULL) {
    error = NW_ERR_ALLOC;
    goto cleanup;
  }

  int draining = 0;
  for (;;) {
    if (!draining) {
      const int read = av_read_frame(fmt_ctx, packet);
      if (read < 0) {
        draining = 1;
        avcodec_send_packet(dec_ctx, NULL);
      } else if (packet->stream_index != stream_index) {
        av_packet_unref(packet);
        continue;
      } else {
        const int sent = avcodec_send_packet(dec_ctx, packet);
        av_packet_unref(packet);
        if (sent < 0 && sent != AVERROR(EAGAIN)) {
          // A corrupt packet should not throw away everything decoded so far.
          continue;
        }
      }
    }

    for (;;) {
      if (avcodec_receive_frame(dec_ctx, frame) < 0) break;

      const enum AVSampleFormat format = (enum AVSampleFormat)frame->format;
      const int planar = av_sample_fmt_is_planar(format);
      const int channels = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 1;
      const int nb_samples = frame->nb_samples;

      int pos = 0;
      while (pos < nb_samples) {
        int take = (int)(bucket_samples - bucket_filled);
        if (take > nb_samples - pos) take = nb_samples - pos;
        if (take <= 0) break;

        switch (format) {
          case AV_SAMPLE_FMT_U8:
          case AV_SAMPLE_FMT_U8P:
            NW_ACCUMULATE(uint8_t, ((double)p[i] - 128.0) * (1.0 / 128.0));
            break;
          case AV_SAMPLE_FMT_S16:
          case AV_SAMPLE_FMT_S16P:
            NW_ACCUMULATE(int16_t, (double)p[i] * (1.0 / 32768.0));
            break;
          case AV_SAMPLE_FMT_S32:
          case AV_SAMPLE_FMT_S32P:
            NW_ACCUMULATE(int32_t, (double)p[i] * (1.0 / 2147483648.0));
            break;
          case AV_SAMPLE_FMT_S64:
          case AV_SAMPLE_FMT_S64P:
            NW_ACCUMULATE(int64_t, (double)p[i] * (1.0 / 9223372036854775808.0));
            break;
          case AV_SAMPLE_FMT_FLT:
          case AV_SAMPLE_FMT_FLTP:
            NW_ACCUMULATE(float, (double)p[i]);
            break;
          case AV_SAMPLE_FMT_DBL:
          case AV_SAMPLE_FMT_DBLP:
            NW_ACCUMULATE(double, p[i]);
            break;
          default:
            error = NW_ERR_UNSUPPORTED_FORMAT;
            av_frame_unref(frame);
            goto cleanup;
        }

        bucket_filled += take;
        pos += take;

        if (bucket_filled >= bucket_samples) {
          const double mean = sum / (double)(bucket_samples * channels);
          if (!nw_buffer_push(&buffer, (float)(sqrt(mean) * 100.0))) {
            error = NW_ERR_ALLOC;
            av_frame_unref(frame);
            goto cleanup;
          }
          sum = 0.0;
          bucket_filled = 0;
        }
      }

      av_frame_unref(frame);
    }

    // Once flushed, the inner loop only exits with nothing left to receive.
    if (draining) break;
  }

  // Flush whatever is left in the last partial bucket.
  if (bucket_filled > 0) {
    const int channels = dec_ctx->ch_layout.nb_channels > 0 ? dec_ctx->ch_layout.nb_channels : 1;
    const double mean = sum / (double)(bucket_filled * channels);
    nw_buffer_push(&buffer, (float)(sqrt(mean) * 100.0));
  }

  if (buffer.count == 0 && error == NW_OK) error = NW_ERR_NO_OUTPUT;

cleanup:
  if (packet != NULL) av_packet_free(&packet);
  if (frame != NULL) av_frame_free(&frame);
  if (dec_ctx != NULL) avcodec_free_context(&dec_ctx);
  if (fmt_ctx != NULL) avformat_close_input(&fmt_ctx);

  result->values = buffer.values;
  result->count = buffer.count;
  result->error = error;
  return result;
}
