// by claude
#include "nw_fft.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NW_FFT_PI 3.14159265358979323846

int nw_fft_init(NWFft* fft, int size) {
  memset(fft, 0, sizeof(*fft));
  const int half = size / 2;
  fft->size = size;
  fft->half = half;

  fft->reversed = (int*)malloc((size_t)half * sizeof(int));
  fft->stage_cos = (float*)malloc((size_t)half * sizeof(float));
  fft->stage_sin = (float*)malloc((size_t)half * sizeof(float));
  fft->turn_cos = (float*)malloc((size_t)half * sizeof(float));
  fft->turn_sin = (float*)malloc((size_t)half * sizeof(float));
  fft->re = (float*)malloc((size_t)half * sizeof(float));
  fft->im = (float*)malloc((size_t)half * sizeof(float));
  if (fft->reversed == NULL || fft->stage_cos == NULL || fft->stage_sin == NULL || fft->turn_cos == NULL || fft->turn_sin == NULL ||
      fft->re == NULL || fft->im == NULL) {
    return 0;
  }

  int bits = 0;
  while ((1 << bits) < half) bits++;
  for (int i = 0; i < half; i++) {
    int reversed = 0;
    for (int b = 0; b < bits; b++) {
      if (i & (1 << b)) reversed |= 1 << (bits - 1 - b);
    }
    fft->reversed[i] = reversed;
  }

  fft->stage_cos[0] = 1.0f;
  fft->stage_sin[0] = 0.0f;
  for (int span = 2; span <= half; span <<= 1) {
    const int reach = span / 2;
    for (int j = 0; j < reach; j++) {
      const double angle = -2.0 * NW_FFT_PI * j / span;
      fft->stage_cos[reach + j] = (float)cos(angle);
      fft->stage_sin[reach + j] = (float)sin(angle);
    }
  }

  for (int k = 0; k < half; k++) {
    const double angle = -2.0 * NW_FFT_PI * k / size;
    fft->turn_cos[k] = (float)cos(angle);
    fft->turn_sin[k] = (float)sin(angle);
  }
  return 1;
}

void nw_fft_free(NWFft* fft) {
  free(fft->reversed);
  free(fft->stage_cos);
  free(fft->stage_sin);
  free(fft->turn_cos);
  free(fft->turn_sin);
  free(fft->re);
  free(fft->im);
  memset(fft, 0, sizeof(*fft));
}

void nw_fft_forward(NWFft* fft, const float* x, const float* window) {
  const int half = fft->half;
  float* re = fft->re;
  float* im = fft->im;
  const int* reversed = fft->reversed;

  for (int i = 0; i < half; i++) {
    const int target = reversed[i];
    re[target] = x[2 * i] * window[2 * i];
    im[target] = x[2 * i + 1] * window[2 * i + 1];
  }

  // -- the first stage turns nothing, so it needs no multiply
  for (int i = 0; i + 1 < half; i += 2) {
    const float ar = re[i], ai = im[i];
    const float br = re[i + 1], bi = im[i + 1];
    re[i] = ar + br;
    im[i] = ai + bi;
    re[i + 1] = ar - br;
    im[i + 1] = ai - bi;
  }

  for (int span = 4; span <= half; span <<= 1) {
    const int reach = span / 2;
    const float* wr = fft->stage_cos + reach;
    const float* wi = fft->stage_sin + reach;
    for (int start = 0; start < half; start += span) {
      float* ar = re + start;
      float* ai = im + start;
      float* br = ar + reach;
      float* bi = ai + reach;
      for (int j = 0; j < reach; j++) {
        const float tr = br[j] * wr[j] - bi[j] * wi[j];
        const float ti = br[j] * wi[j] + bi[j] * wr[j];
        br[j] = ar[j] - tr;
        bi[j] = ai[j] - ti;
        ar[j] += tr;
        ai[j] += ti;
      }
    }
  }
}
