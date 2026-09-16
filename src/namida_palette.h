#ifndef NAMIDA_PALETTE_H
#define NAMIDA_PALETTE_H

#include <stdint.h>

#if defined(_WIN32)
#define NP_EXPORT __declspec(dllexport)
#else
#define NP_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Error codes reported through `NPResult.error`.
enum {
  NP_OK = 0,
  NP_ERR_ALLOC = 1,
  NP_ERR_OPEN_INPUT = 2,
  NP_ERR_NO_VIDEO_STREAM = 3,
  NP_ERR_NO_DECODER = 4,
  NP_ERR_DECODER_OPEN = 5,
  NP_ERR_DECODE = 6,
  NP_ERR_UNSUPPORTED_FORMAT = 7,
  NP_ERR_NO_OUTPUT = 8,
  NP_ERR_THREAD = 9,
};

/// Result of a single extraction. Always free with `np_result_free`.
typedef struct {
  /// `0xAARRGGBB` colors, alpha always `0xFF`, most populous first. Owned by
  /// the library, valid until `np_result_free`.
  uint32_t* colors;
  /// How many sampled pixels fell into each entry of `colors`.
  uint32_t* populations;
  /// Number of valid entries in `colors` and `populations`.
  int32_t count;
  /// `NP_OK` on success, one of the `NP_ERR_*` codes otherwise.
  int32_t error;
  /// Size of the decoded picture the palette was sampled from, which for JPEG
  /// may be a power-of-two fraction of the file's size.
  int32_t width;
  int32_t height;
} NPResult;

/// Decodes one picture and reduces it to at most `max_colors` colors with the
/// median-cut quantizer Android's Palette uses. The picture is box-averaged
/// onto a grid at most `max_height` rows tall, its width following the aspect
/// ratio, so every source pixel weighs in on the histogram.
///
/// The picture comes from `data` when it is non-NULL, otherwise from `path`.
/// Every format libavcodec was built with is accepted; the container is
/// detected from the bytes, never from the file name.
///
/// Blocks the calling thread. Returns NULL only when the result struct itself
/// could not be allocated.
NP_EXPORT NPResult* np_extract(const char* path, const uint8_t* data, int64_t data_size, int32_t max_colors,
                               int32_t max_height);

/// Receives the result of `np_extract_async` on the worker thread. The result
/// is owned by the receiver, which frees it with `np_result_free`; it is NULL
/// only when the result struct could not be allocated.
typedef void (*np_callback)(int64_t request_id, NPResult* result);

/// `np_extract` on a detached thread of its own. `path` and `data` are copied
/// before this returns, so the caller can release them right away.
///
/// Returns `NP_OK` once the thread is running, in which case `callback` is
/// invoked exactly once, or an `NP_ERR_*` code when nothing was started.
NP_EXPORT int32_t np_extract_async(const char* path, const uint8_t* data, int64_t data_size, int32_t max_colors,
                                   int32_t max_height, int64_t request_id, np_callback callback);

/// Releases a result returned by `np_extract` or handed to a callback. Safe to
/// call with NULL.
NP_EXPORT void np_result_free(NPResult* result);

/// Version of the palette API, for diagnostics.
NP_EXPORT int32_t np_version(void);

#ifdef __cplusplus
}
#endif

#endif  // NAMIDA_PALETTE_H
