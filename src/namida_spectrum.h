#ifndef NAMIDA_SPECTRUM_H
#define NAMIDA_SPECTRUM_H

#include <stdint.h>

#include "namida_waveform.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Result of a single extraction. Always free with `ns_result_free`.
/// Errors are the `NW_ERR_*` codes of `namida_waveform.h`.
typedef struct {
  /// `frame_count` rows of `band_count + 1` bytes: the level of every band, low
  /// to high, then how hard a beat lands on that frame. Owned by the library,
  /// valid until `ns_result_free`.
  uint8_t* data;
  int32_t frame_count;
  int32_t band_count;
  int32_t frames_per_second;
  /// `NW_OK` on success. When non-zero `data` may still hold a partial result
  /// (decoding stopped early).
  int32_t error;
  /// Source duration in milliseconds, `0` when the container reports none.
  int64_t duration_ms;
  /// Decoder output sample rate, `0` when extraction failed before opening it.
  int32_t sample_rate;
} NSResult;

/// Decodes `path` and reduces it to `frames_per_second` rows of `band_count`
/// log spaced frequency band levels, each followed by a beat strength.
///
/// Frame `i` is centered on `i / frames_per_second` seconds, so a row can be
/// looked up by playback position alone. Levels are relative to the loudest
/// band of the track rather than to full scale, a quiet recording moves as
/// much as a loud one.
///
/// Returns NULL only when the result struct itself could not be allocated.
NW_EXPORT NSResult* ns_extract(const char* path, int32_t frames_per_second, int32_t band_count);

/// Releases a result returned by `ns_extract`. Safe to call with NULL.
NW_EXPORT void ns_result_free(NSResult* result);

/// Version of the spectrum output, bumped whenever the same input would
/// produce different rows.
NW_EXPORT int32_t ns_version(void);

#ifdef __cplusplus
}
#endif

#endif  // NAMIDA_SPECTRUM_H
