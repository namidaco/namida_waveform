// by claude
#ifndef NL_ANALYZER_H
#define NL_ANALYZER_H

#include <stdint.h>

#include "namida_lossless.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Average spectrum of a stream of mono samples and where it cuts off.
/// Internal to the library and free of libav, so any decoder can drive it.
typedef struct NLAnalyzer NLAnalyzer;

/// Returns NULL when allocating failed.
NLAnalyzer* nl_analyzer_create(int sample_rate);

/// Takes `count` mono samples in the `-1..1` domain. Blocks never span two
/// calls' gaps: call `nl_analyzer_break` between slices that are not
/// contiguous.
void nl_analyzer_feed(NLAnalyzer* analyzer, const float* samples, int count);

/// Drops the partial block, the next samples start a new one.
void nl_analyzer_break(NLAnalyzer* analyzer);

/// Fills `cutoff_hz` and `cutoff_drop_db` of `result`.
void nl_analyzer_finish(NLAnalyzer* analyzer, NLResult* result);

void nl_analyzer_free(NLAnalyzer* analyzer);

#ifdef __cplusplus
}
#endif

#endif  // NL_ANALYZER_H
