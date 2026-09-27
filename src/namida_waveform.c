#include "namida_waveform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nw_decoder.h"

#define NW_VERSION 2

#define NW_MIN_SPS 1
#define NW_MAX_SPS 1000
#define NW_DEFAULT_SPS 100

/// Defines `NAME(p, n)`: the sum of squares of `n` contiguous samples, spread
/// over four independent accumulators so consecutive adds do not serialise on
/// one dependency chain. Integer formats accumulate exactly; the caller scales
/// the total to the `-1..1` domain once, instead of per sample.
#define NW_DEFINE_SUM_SQUARES(NAME, TYPE, ACC, SQUARE) \
  static inline double NAME(const TYPE* p, int n) {    \
    ACC s0 = 0, s1 = 0, s2 = 0, s3 = 0;                \
    int i = 0;                                         \
    for (; i + 4 <= n; i += 4) {                       \
      s0 += SQUARE(p[i]);                              \
      s1 += SQUARE(p[i + 1]);                          \
      s2 += SQUARE(p[i + 2]);                          \
      s3 += SQUARE(p[i + 3]);                          \
    }                                                  \
    for (; i < n; i++) s0 += SQUARE(p[i]);             \
    return (double)((s0 + s1) + (s2 + s3));            \
  }

#define NW_SQUARE_U8(x) ((int64_t)(((int32_t)(x) - 128) * ((int32_t)(x) - 128)))
#define NW_SQUARE_S16(x) ((int64_t)((int32_t)(x) * (int32_t)(x)))
#define NW_SQUARE_DOUBLE(x) ((double)(x) * (double)(x))

NW_DEFINE_SUM_SQUARES(nw_sum_squares_u8, uint8_t, int64_t, NW_SQUARE_U8)
NW_DEFINE_SUM_SQUARES(nw_sum_squares_s16, int16_t, int64_t, NW_SQUARE_S16)
NW_DEFINE_SUM_SQUARES(nw_sum_squares_s32, int32_t, double, NW_SQUARE_DOUBLE)
NW_DEFINE_SUM_SQUARES(nw_sum_squares_s64, int64_t, double, NW_SQUARE_DOUBLE)
NW_DEFINE_SUM_SQUARES(nw_sum_squares_flt, float, double, NW_SQUARE_DOUBLE)
NW_DEFINE_SUM_SQUARES(nw_sum_squares_dbl, double, double, NW_SQUARE_DOUBLE)

#define NW_SCALE_U8 (1.0 / (128.0 * 128.0))
#define NW_SCALE_S16 (1.0 / (32768.0 * 32768.0))
#define NW_SCALE_S32 (1.0 / (2147483648.0 * 2147483648.0))
#define NW_SCALE_S64 (1.0 / (9223372036854775808.0 * 9223372036854775808.0))

/// Adds the squares of `take` sample positions starting at `pos`, over every
/// channel, to `sum`. Planar data is one contiguous run per channel, packed
/// data one interleaved run, so both stay a flat walk over memory.
#define NW_ACCUMULATE(SUM_SQUARES, TYPE, SCALE)                                        \
  do {                                                                                 \
    if (planar) {                                                                      \
      for (int ch = 0; ch < channels; ch++) {                                          \
        const TYPE* p = ((const TYPE*)frame->extended_data[ch]) + pos;                 \
        sum += SUM_SQUARES(p, take) * (SCALE);                                         \
      }                                                                                \
    } else {                                                                           \
      const TYPE* p = ((const TYPE*)frame->extended_data[0]) + (size_t)pos * channels; \
      sum += SUM_SQUARES(p, take * channels) * (SCALE);                                \
    }                                                                                  \
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

NW_EXPORT int32_t nw_version(void) { return NW_VERSION; }

NW_EXPORT void nw_result_free(NWResult* result) {
  if (result == NULL) return;
  free(result->values);
  free(result);
}

NW_EXPORT NWResult* nw_extract(const char* path, int32_t samples_per_second) {
  NWResult* result = (NWResult*)calloc(1, sizeof(NWResult));
  if (result == NULL) return NULL;

  if (samples_per_second <= 0) samples_per_second = NW_DEFAULT_SPS;
  if (samples_per_second < NW_MIN_SPS) samples_per_second = NW_MIN_SPS;
  if (samples_per_second > NW_MAX_SPS) samples_per_second = NW_MAX_SPS;

  NWDecoder decoder;
  NWBuffer buffer = {NULL, 0, 0};

  double sum = 0.0;
  int64_t bucket_filled = 0;
  int64_t bucket_samples = 0;
  int channels = 1;

  int32_t error = nw_decoder_open(&decoder, path);
  if (error != NW_OK) goto cleanup;

  result->duration_ms = decoder.duration_ms;

  // The container's idea of the rate is only a fallback: the decoder reports
  // the real one on the first frame, and raw streams have no container at all.
  result->sample_rate = decoder.dec_ctx->sample_rate;
  result->channels = decoder.dec_ctx->ch_layout.nb_channels;

  if (result->duration_ms > 0) {
    const int64_t estimate = (result->duration_ms * samples_per_second) / 1000 + 64;
    if (estimate > 0 && estimate < INT32_MAX && !nw_buffer_reserve(&buffer, (int32_t)estimate)) {
      error = NW_ERR_ALLOC;
      goto cleanup;
    }
  }

  while (nw_decoder_next(&decoder)) {
    const AVFrame* frame = decoder.frame;
    const enum AVSampleFormat format = (enum AVSampleFormat)frame->format;
    const int planar = av_sample_fmt_is_planar(format);
    const int nb_samples = frame->nb_samples;
    channels = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 1;

    if (bucket_samples == 0) {
      const int sample_rate = frame->sample_rate > 0 ? frame->sample_rate : decoder.dec_ctx->sample_rate;
      result->sample_rate = sample_rate > 0 ? sample_rate : 44100;
      result->channels = channels;
      bucket_samples = result->sample_rate / samples_per_second;
      if (bucket_samples < 1) bucket_samples = 1;
    }

    int pos = 0;
    while (pos < nb_samples) {
      int take = (int)(bucket_samples - bucket_filled);
      if (take > nb_samples - pos) take = nb_samples - pos;

      switch (format) {
        case AV_SAMPLE_FMT_U8:
        case AV_SAMPLE_FMT_U8P:
          NW_ACCUMULATE(nw_sum_squares_u8, uint8_t, NW_SCALE_U8);
          break;
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P:
          NW_ACCUMULATE(nw_sum_squares_s16, int16_t, NW_SCALE_S16);
          break;
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
          NW_ACCUMULATE(nw_sum_squares_s32, int32_t, NW_SCALE_S32);
          break;
        case AV_SAMPLE_FMT_S64:
        case AV_SAMPLE_FMT_S64P:
          NW_ACCUMULATE(nw_sum_squares_s64, int64_t, NW_SCALE_S64);
          break;
        case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_FLTP:
          NW_ACCUMULATE(nw_sum_squares_flt, float, 1.0);
          break;
        case AV_SAMPLE_FMT_DBL:
        case AV_SAMPLE_FMT_DBLP:
          NW_ACCUMULATE(nw_sum_squares_dbl, double, 1.0);
          break;
        default:
          error = NW_ERR_UNSUPPORTED_FORMAT;
          goto cleanup;
      }

      bucket_filled += take;
      pos += take;

      if (bucket_filled >= bucket_samples) {
        const double mean = sum / (double)(bucket_samples * channels);
        if (!nw_buffer_push(&buffer, (float)(sqrt(mean) * 100.0))) {
          error = NW_ERR_ALLOC;
          goto cleanup;
        }
        sum = 0.0;
        bucket_filled = 0;
      }
    }
  }

  // Flush whatever is left in the last partial bucket.
  if (bucket_filled > 0) {
    const double mean = sum / (double)(bucket_filled * channels);
    nw_buffer_push(&buffer, (float)(sqrt(mean) * 100.0));
  }

  if (buffer.count == 0 && error == NW_OK) error = NW_ERR_NO_OUTPUT;

cleanup:
  nw_decoder_close(&decoder);

  result->values = buffer.values;
  result->count = buffer.count;
  result->error = error;
  return result;
}
