#ifndef NAMIDA_RHYTHM_H
#define NAMIDA_RHYTHM_H

#include <stdint.h>

#include "namida_waveform.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Result of a single analysis. Always free with `nr_result_free`.
/// Errors are the `NW_ERR_*` codes of `namida_waveform.h`.
typedef struct {
  /// Beats per minute of the steady grid fitted over the whole track, `0` when
  /// no steady beat was found.
  double bpm;
  /// Position of a beat of that grid within the first beat period, in
  /// milliseconds. Every other beat sits a whole number of periods away.
  double beat_offset_ms;
  /// How well one steady grid fits the whole track, `0..1`. Tracks drifting in
  /// tempo, or with no clear pulse, score low.
  float beat_confidence;
  /// `0..11` for the major keys C to B, `12..23` for the minor keys C to B,
  /// `-1` when the track carries no pitch to tell.
  int32_t key;
  /// Correlation of the pitch classes of the track with the profile of `key`,
  /// `0..1`.
  float key_confidence;
  /// `NW_OK` on success. When non-zero the fields above may still describe the
  /// part decoded before the error.
  int32_t error;
  /// Where the audio first rises above, and last falls below, silence, in
  /// milliseconds.
  int64_t audible_start_ms;
  int64_t audible_end_ms;
  /// Where the track settles into its fade out or quiet outro, in
  /// milliseconds. `audible_end_ms` when it ends at its usual loudness.
  int64_t fade_out_start_ms;
  /// Source duration in milliseconds, `0` when the container reports none.
  int64_t duration_ms;
  /// Decoder output sample rate, `0` when analysis failed before opening it.
  int32_t sample_rate;
} NRResult;

/// Decodes `path` and finds its tempo, beat grid and key.
///
/// `bpm_hint` is a tempo known from elsewhere, such as a tag, `0` for none. It
/// settles which of the tempos a beat implies (half, double) is reported, the
/// exact value still comes from the audio.
///
/// Returns NULL only when the result struct itself could not be allocated.
NW_EXPORT NRResult* nr_analyze(const char* path, double bpm_hint);

/// Releases a result returned by `nr_analyze`. Safe to call with NULL.
NW_EXPORT void nr_result_free(NRResult* result);

/// Version of the analysis, bumped whenever the same input would give a
/// different result.
NW_EXPORT int32_t nr_version(void);

#ifdef __cplusplus
}
#endif

#endif  // NAMIDA_RHYTHM_H
