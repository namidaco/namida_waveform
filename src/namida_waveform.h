#ifndef NAMIDA_WAVEFORM_H
#define NAMIDA_WAVEFORM_H

#include <stdint.h>

#if defined(_WIN32)
#define NW_EXPORT __declspec(dllexport)
#else
#define NW_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Error codes reported through `NWResult.error`.
enum {
  NW_OK = 0,
  NW_ERR_ALLOC = 1,
  NW_ERR_OPEN_INPUT = 2,
  NW_ERR_STREAM_INFO = 3,
  NW_ERR_NO_AUDIO_STREAM = 4,
  NW_ERR_NO_DECODER = 5,
  NW_ERR_DECODER_OPEN = 6,
  NW_ERR_UNSUPPORTED_FORMAT = 7,
  NW_ERR_NO_OUTPUT = 8,
};

/// Result of a single extraction. Always free with `nw_result_free`.
typedef struct {
  /// RMS amplitudes in the `0..100` range, one per requested sample slot.
  /// Owned by the library, valid until `nw_result_free`.
  float* values;
  /// Number of valid entries in `values`.
  int32_t count;
  /// `NW_OK` on success, one of the `NW_ERR_*` codes otherwise. When non-zero
  /// `values` may still hold a partial result (decoding stopped early).
  int32_t error;
  /// Source duration in milliseconds, `0` when the container reports none.
  int64_t duration_ms;
  /// Decoder output sample rate, `0` when extraction failed before opening it.
  int32_t sample_rate;
  /// Decoder output channel count, `0` when extraction failed before opening it.
  int32_t channels;
} NWResult;

/// Decodes `path` and reduces it to `samples_per_second` RMS amplitudes per
/// second of audio.
///
/// Buckets are cut on sample positions rather than decoder frame boundaries, so
/// the output rate is identical for every codec and container.
///
/// Returns NULL only when the result struct itself could not be allocated.
NW_EXPORT NWResult* nw_extract(const char* path, int32_t samples_per_second);

/// Releases a result returned by `nw_extract`. Safe to call with NULL.
NW_EXPORT void nw_result_free(NWResult* result);

/// Version of this library, for diagnostics.
NW_EXPORT int32_t nw_version(void);

#ifdef __cplusplus
}
#endif

#endif  // NAMIDA_WAVEFORM_H
