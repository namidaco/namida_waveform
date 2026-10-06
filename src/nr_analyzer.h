// by claude
#ifndef NR_ANALYZER_H
#define NR_ANALYZER_H

#include <stdint.h>

#include "namida_rhythm.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Tempo, beat grid and key of a stream of mono samples. Internal to the
/// library and free of libav, so any decoder can drive it.
typedef struct NRAnalyzer NRAnalyzer;

/// Returns NULL when allocating failed.
NRAnalyzer* nr_analyzer_create(int sample_rate);

/// Takes `count` mono samples in the `-1..1` domain. Returns `NW_OK` or
/// `NW_ERR_ALLOC`.
int32_t nr_analyzer_feed(NRAnalyzer* analyzer, const float* samples, int count);

/// Fills the tempo, beat grid, key and audible range of `result` from
/// everything fed so far. Returns `NW_OK`, `NW_ERR_ALLOC` or `NW_ERR_NO_OUTPUT`.
int32_t nr_analyzer_finish(NRAnalyzer* analyzer, double bpm_hint, NRResult* result);

void nr_analyzer_free(NRAnalyzer* analyzer);

#ifdef __cplusplus
}
#endif

#endif  // NR_ANALYZER_H
