// by claude
#include "namida_lossless.h"

#include <stdlib.h>

#include "nl_analyzer.h"
#include "nw_decoder.h"

#define NL_VERSION 1

/// Slices spread over the track, each this long, are enough to average its
/// spectrum, and they sit between these shares of the duration, past intros and
/// fade outs.
#define NL_SLICES 16
#define NL_SLICE_SECONDS 3.0
#define NL_FIRST_SLICE_AT 0.05
#define NL_LAST_SLICE_AT 0.9

/// Read from the start instead when there is no duration to spread slices over,
/// or the input can't seek.
#define NL_UNSEEKABLE_SECONDS 60.0

typedef struct {
  NLAnalyzer* analyzer;
  float* mono;
  int mono_capacity;
  /// Every integer sample OR-ed together, to tell which low bits never carry
  /// anything.
  uint64_t sample_bits;
  int container_bits;
} NLState;

#define NL_COLLECT_BITS(TYPE, UNSIGNED, BITS)                            \
  do {                                                                   \
    state->container_bits = BITS;                                        \
    for (int plane = 0; plane < planes; plane++) {                       \
      const TYPE* p = (const TYPE*)frame->extended_data[plane];          \
      for (int i = 0; i < per_plane; i++) bits |= (UNSIGNED)p[i];        \
    }                                                                    \
  } while (0)

static void nl_collect_bits(NLState* state, const AVFrame* frame) {
  const enum AVSampleFormat format = (enum AVSampleFormat)frame->format;
  const int channels = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : 1;
  const int planar = av_sample_fmt_is_planar(format);
  const int planes = planar ? channels : 1;
  const int per_plane = planar ? frame->nb_samples : frame->nb_samples * channels;
  uint64_t bits = 0;
  switch (format) {
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
      NL_COLLECT_BITS(int16_t, uint16_t, 16);
      break;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
      NL_COLLECT_BITS(int32_t, uint32_t, 32);
      break;
    default:
      return;
  }
  state->sample_bits |= bits;
}

static int nl_used_bits(uint64_t bits, int container_bits) {
  if (bits == 0 || container_bits == 0) return 0;
  int unused = 0;
  while ((bits & 1) == 0) {
    bits >>= 1;
    unused++;
  }
  return container_bits - unused;
}

static int32_t nl_take_frame(NLState* state, const AVFrame* frame) {
  const int n = frame->nb_samples;
  if (n <= 0) return NW_OK;
  if (n > state->mono_capacity) {
    float* grown = (float*)realloc(state->mono, (size_t)n * sizeof(float));
    if (grown == NULL) return NW_ERR_ALLOC;
    state->mono = grown;
    state->mono_capacity = n;
  }
  const int32_t error = nw_decoder_downmix(frame, state->mono);
  if (error != NW_OK) return error;
  nl_analyzer_feed(state->analyzer, state->mono, n);
  nl_collect_bits(state, frame);
  return NW_OK;
}

/// Reads frames until `samples` were taken or the stream ended.
static int32_t nl_read(NLState* state, NWDecoder* decoder, int64_t samples) {
  int64_t taken = 0;
  while (taken < samples && nw_decoder_next(decoder)) {
    const int32_t error = nl_take_frame(state, decoder->frame);
    if (error != NW_OK) return error;
    taken += decoder->frame->nb_samples;
  }
  return NW_OK;
}

NW_EXPORT int32_t nl_version(void) { return NL_VERSION; }

NW_EXPORT void nl_result_free(NLResult* result) { free(result); }

NW_EXPORT NLResult* nl_check(const char* path) {
  NLResult* result = (NLResult*)calloc(1, sizeof(NLResult));
  if (result == NULL) return NULL;

  NWDecoder decoder;
  NLState state = {0};

  int32_t error = nw_decoder_open(&decoder, path);
  if (error != NW_OK) goto cleanup;

  result->duration_ms = decoder.duration_ms;
  const AVCodecParameters* parameters = decoder.fmt_ctx->streams[decoder.stream_index]->codecpar;
  const AVCodecDescriptor* descriptor = avcodec_descriptor_get(parameters->codec_id);
  result->lossless = descriptor != NULL && (descriptor->props & AV_CODEC_PROP_LOSSLESS) && !(descriptor->props & AV_CODEC_PROP_LOSSY);
  result->stored_bits = parameters->bits_per_raw_sample > 0 ? parameters->bits_per_raw_sample : parameters->bits_per_coded_sample;
  // -- only what claims to be lossless can be a fake one
  if (!result->lossless) goto cleanup;

  int sample_rate = decoder.dec_ctx->sample_rate;
  if (sample_rate <= 0) sample_rate = 44100;
  result->sample_rate = sample_rate;
  state.analyzer = nl_analyzer_create(sample_rate);
  if (state.analyzer == NULL) {
    error = NW_ERR_ALLOC;
    goto cleanup;
  }

  const int64_t slice_samples = (int64_t)(NL_SLICE_SECONDS * sample_rate);
  int slices_read = 0;
  if (decoder.duration_ms > 0) {
    for (int s = 0; s < NL_SLICES; s++) {
      const double share = NL_FIRST_SLICE_AT + (NL_LAST_SLICE_AT - NL_FIRST_SLICE_AT) * s / (NL_SLICES - 1);
      const int64_t at_ms = (int64_t)(decoder.duration_ms * share);
      if (!nw_decoder_seek(&decoder, at_ms)) break;
      nl_analyzer_break(state.analyzer);
      error = nl_read(&state, &decoder, slice_samples);
      if (error != NW_OK) goto cleanup;
      slices_read++;
    }
  }
  if (slices_read == 0) {
    const int64_t unseekable_samples = (int64_t)(NL_UNSEEKABLE_SECONDS * sample_rate);
    error = nl_read(&state, &decoder, unseekable_samples);
    if (error != NW_OK) goto cleanup;
  }

  nl_analyzer_finish(state.analyzer, result);
  result->used_bits = nl_used_bits(state.sample_bits, state.container_bits);

cleanup:
  nl_analyzer_free(state.analyzer);
  free(state.mono);
  nw_decoder_close(&decoder);
  result->error = error;
  return result;
}
