// by claude
#ifndef NW_FFT_H
#define NW_FFT_H

#ifdef __cplusplus
extern "C" {
#endif

/// A real transform of `size` samples folded into a complex one of half the
/// size: even samples go to the real part, odd ones to the imaginary part, and
/// the two are told apart again per bin, only for the bins a caller reads.
/// Shared by the spectrum and rhythm extractors, internal to the library.
typedef struct {
  int size;
  int half;
  int* reversed;
  /// Twiddles of every stage back to back, the stage spanning `s` samples
  /// sits at `[s / 2, s)`, so each stage walks its own contiguous run.
  float* stage_cos;
  float* stage_sin;
  /// Unfolds the half size transform back into the real one.
  float* turn_cos;
  float* turn_sin;
  float* re;
  float* im;
} NWFft;

/// `size` is a power of two, at least 4. Returns 0 when allocating failed,
/// `nw_fft_free` has to be called either way.
int nw_fft_init(NWFft* fft, int size);

void nw_fft_free(NWFft* fft);

/// Transforms the `size` samples at `x`, each multiplied by `window[i]`.
void nw_fft_forward(NWFft* fft, const float* x, const float* window);

/// Squared magnitude of bin `k` of the last `nw_fft_forward`, `k` in `[1, half)`.
static inline float nw_fft_bin_power(const NWFft* fft, int k) {
  const float* re = fft->re;
  const float* im = fft->im;
  const int mirror = fft->half - k;
  const float ar = re[k], ai = im[k];
  const float br = re[mirror], bi = -im[mirror];
  const float even_r = 0.5f * (ar + br);
  const float even_i = 0.5f * (ai + bi);
  const float odd_r = 0.5f * (ai - bi);
  const float odd_i = -0.5f * (ar - br);
  const float c = fft->turn_cos[k], sn = fft->turn_sin[k];
  const float xr = even_r + c * odd_r - sn * odd_i;
  const float xi = even_i + c * odd_i + sn * odd_r;
  return xr * xr + xi * xi;
}

#ifdef __cplusplus
}
#endif

#endif  // NW_FFT_H
