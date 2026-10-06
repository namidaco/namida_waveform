// by claude
#include "namida_rhythm.h"

#include <stdlib.h>

#include "nr_analyzer.h"
#include "nw_decoder.h"

#define NR_VERSION 1

NW_EXPORT int32_t nr_version(void) { return NR_VERSION; }

NW_EXPORT void nr_result_free(NRResult* result) { free(result); }

NW_EXPORT NRResult* nr_analyze(const char* path, double bpm_hint) {
  NRResult* result = (NRResult*)calloc(1, sizeof(NRResult));
  if (result == NULL) return NULL;
  result->key = -1;

  NWDecoder decoder;
  NRAnalyzer* analyzer = NULL;
  float* mono = NULL;
  int mono_capacity = 0;

  int32_t error = nw_decoder_open(&decoder, path);
  if (error != NW_OK) goto cleanup;

  result->duration_ms = decoder.duration_ms;
  result->sample_rate = decoder.dec_ctx->sample_rate;

  while (nw_decoder_next(&decoder)) {
    const AVFrame* frame = decoder.frame;
    if (analyzer == NULL) {
      // The container's idea of the rate is only a fallback: the decoder
      // reports the real one on the first frame.
      int sample_rate = frame->sample_rate > 0 ? frame->sample_rate : decoder.dec_ctx->sample_rate;
      if (sample_rate <= 0) sample_rate = 44100;
      result->sample_rate = sample_rate;
      analyzer = nr_analyzer_create(sample_rate);
      if (analyzer == NULL) {
        error = NW_ERR_ALLOC;
        goto cleanup;
      }
    }

    const int n = frame->nb_samples;
    if (n <= 0) continue;
    if (n > mono_capacity) {
      float* grown = (float*)realloc(mono, (size_t)n * sizeof(float));
      if (grown == NULL) {
        error = NW_ERR_ALLOC;
        goto cleanup;
      }
      mono = grown;
      mono_capacity = n;
    }
    error = nw_decoder_downmix(frame, mono);
    if (error != NW_OK) goto cleanup;
    error = nr_analyzer_feed(analyzer, mono, n);
    if (error != NW_OK) goto cleanup;
  }

  if (analyzer == NULL) {
    error = NW_ERR_NO_OUTPUT;
    goto cleanup;
  }

cleanup:
  if (analyzer != NULL) {
    // -- a decode that stopped early still says what it read so far
    const int32_t finished = nr_analyzer_finish(analyzer, bpm_hint, result);
    if (error == NW_OK) error = finished;
  }
  nr_analyzer_free(analyzer);
  free(mono);
  nw_decoder_close(&decoder);
  result->error = error;
  return result;
}
