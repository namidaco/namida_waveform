#ifndef NAMIDA_LOSSLESS_H
#define NAMIDA_LOSSLESS_H

#include <stdint.h>

#include "namida_waveform.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Result of a single check. Always free with `nl_result_free`.
/// Errors are the `NW_ERR_*` codes of `namida_waveform.h`.
typedef struct {
  /// Highest frequency the audio carries above its noise floor, in hertz. `0`
  /// when the file is lossy or nothing could be read.
  float cutoff_hz;
  /// How far the level falls across the cutoff, in decibels. A lossy encoder's
  /// lowpass drops tens of decibels at once, music rolling off on its own only
  /// a few.
  float cutoff_drop_db;
  /// `1` when the codec is a lossless one, only those are analyzed.
  int32_t lossless;
  int32_t sample_rate;
  /// Bits per sample the file stores.
  int32_t stored_bits;
  /// Bits per sample that ever carry anything, `0` when unknown. Fewer than
  /// `stored_bits` means the samples were padded.
  int32_t used_bits;
  /// `NW_OK` on success.
  int32_t error;
  /// Source duration in milliseconds, `0` when the container reports none.
  int64_t duration_ms;
} NLResult;

/// Reads slices spread over `path` and measures what a lossless file built from
/// a lossy, lower rate or lower depth source gives away: the lowpass of the
/// lossy encoder, an empty band over the source's rate, and padded sample bits.
///
/// Returns NULL only when the result struct itself could not be allocated.
NW_EXPORT NLResult* nl_check(const char* path);

/// Releases a result returned by `nl_check`. Safe to call with NULL.
NW_EXPORT void nl_result_free(NLResult* result);

/// Version of the check, bumped whenever the same input would give a different
/// result.
NW_EXPORT int32_t nl_version(void);

#ifdef __cplusplus
}
#endif

#endif  // NAMIDA_LOSSLESS_H
