// by claude
#include "nl_analyzer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nw_fft.h"

#define NL_PI 3.14159265358979323846

#define NL_FFT 4096

/// Levels are averaged this far either side of every bin, so single tones and
/// gaps between harmonics don't read as an edge.
#define NL_SMOOTH_HZ 200.0

/// Lossy encoders cut somewhere over this.
#define NL_EDGE_FROM_HZ 8000.0
/// An edge is measured between the band just under it and the band just over
/// it, leaving out the slope in between, and against everything over it so a
/// notch the spectrum climbs back out of is no edge.
#define NL_EDGE_GAP_HZ 300.0
#define NL_EDGE_BAND_HZ 700.0

struct NLAnalyzer {
  int sample_rate;
  NWFft fft;
  float* window;
  float* block;
  int filled;
  /// Power of every bin summed over all blocks.
  double* power;
  int64_t blocks;
};

NLAnalyzer* nl_analyzer_create(int sample_rate) {
  if (sample_rate <= 0) return NULL;
  NLAnalyzer* a = (NLAnalyzer*)calloc(1, sizeof(NLAnalyzer));
  if (a == NULL) return NULL;
  a->sample_rate = sample_rate;
  a->window = (float*)malloc(NL_FFT * sizeof(float));
  a->block = (float*)malloc(NL_FFT * sizeof(float));
  a->power = (double*)calloc(NL_FFT / 2, sizeof(double));
  if (!nw_fft_init(&a->fft, NL_FFT) || a->window == NULL || a->block == NULL || a->power == NULL) {
    nl_analyzer_free(a);
    return NULL;
  }
  for (int i = 0; i < NL_FFT; i++) a->window[i] = (float)(0.5 - 0.5 * cos(2.0 * NL_PI * i / NL_FFT));
  return a;
}

void nl_analyzer_free(NLAnalyzer* a) {
  if (a == NULL) return;
  nw_fft_free(&a->fft);
  free(a->window);
  free(a->block);
  free(a->power);
  free(a);
}

static void nl_add_block(NLAnalyzer* a) {
  nw_fft_forward(&a->fft, a->block, a->window);
  for (int k = 1; k < NL_FFT / 2; k++) a->power[k] += nw_fft_bin_power(&a->fft, k);
  a->blocks++;
}

void nl_analyzer_feed(NLAnalyzer* a, const float* samples, int count) {
  int i = 0;
  while (i < count) {
    int take = NL_FFT - a->filled;
    if (take > count - i) take = count - i;
    memcpy(a->block + a->filled, samples + i, (size_t)take * sizeof(float));
    a->filled += take;
    i += take;
    if (a->filled == NL_FFT) {
      nl_add_block(a);
      a->filled = 0;
    }
  }
}

void nl_analyzer_break(NLAnalyzer* a) { a->filled = 0; }

/// Average level over the bins of `[from_hz, to_hz)`, from the prefix `sums`
/// of the levels, NAN when that holds no bin.
static double nl_band_level(const double* sums, int half, double bin_hz, double from_hz, double to_hz) {
  int from = (int)ceil(from_hz / bin_hz);
  int to = (int)ceil(to_hz / bin_hz);
  if (from < 1) from = 1;
  if (to > half) to = half;
  if (to <= from) return NAN;
  return (sums[to] - sums[from]) / (to - from);
}

void nl_analyzer_finish(NLAnalyzer* a, NLResult* result) {
  result->cutoff_hz = 0.0f;
  result->cutoff_drop_db = 0.0f;
  if (a->blocks == 0) return;

  const int half = NL_FFT / 2;
  const double bin_hz = (double)a->sample_rate / NL_FFT;
  double* db = (double*)malloc((size_t)half * sizeof(double));
  double* sums = (double*)malloc(((size_t)half + 1) * sizeof(double));
  double* smooth = (double*)malloc((size_t)half * sizeof(double));
  if (db == NULL || sums == NULL || smooth == NULL) goto cleanup;

  db[0] = 0.0;
  for (int k = 1; k < half; k++) db[k] = 10.0 * log10(a->power[k] / (double)a->blocks + 1e-30);
  sums[0] = 0.0;
  sums[1] = 0.0;
  for (int k = 1; k < half; k++) sums[k + 1] = sums[k] + db[k];

  int reach = (int)(NL_SMOOTH_HZ / bin_hz);
  if (reach < 1) reach = 1;
  smooth[0] = 0.0;
  for (int k = 1; k < half; k++) {
    const int from = k - reach < 1 ? 1 : k - reach;
    const int to = k + reach + 1 > half ? half : k + reach + 1;
    smooth[k] = (sums[to] - sums[from]) / (to - from);
  }
  // -- the same prefix sums, over the smoothed levels now
  for (int k = 1; k < half; k++) sums[k + 1] = sums[k] + smooth[k];

  const double nyquist_hz = half * bin_hz;
  double best_drop = 0.0;
  double best_hz = 0.0;
  for (int k = (int)ceil(NL_EDGE_FROM_HZ / bin_hz); k < half; k++) {
    const double hz = k * bin_hz;
    if (hz + NL_EDGE_GAP_HZ + NL_EDGE_BAND_HZ > nyquist_hz) break;
    const double below = nl_band_level(sums, half, bin_hz, hz - NL_EDGE_GAP_HZ - NL_EDGE_BAND_HZ, hz - NL_EDGE_GAP_HZ);
    const double above = nl_band_level(sums, half, bin_hz, hz + NL_EDGE_GAP_HZ, hz + NL_EDGE_GAP_HZ + NL_EDGE_BAND_HZ);
    const double rest = nl_band_level(sums, half, bin_hz, hz + NL_EDGE_GAP_HZ, nyquist_hz);
    if (isnan(below) || isnan(above) || isnan(rest)) continue;
    const double local_drop = below - above;
    const double lasting_drop = below - rest;
    const double drop = local_drop < lasting_drop ? local_drop : lasting_drop;
    if (drop > best_drop) {
      best_drop = drop;
      best_hz = hz;
    }
  }
  result->cutoff_hz = (float)(best_hz > 0.0 ? best_hz : nyquist_hz);
  result->cutoff_drop_db = (float)best_drop;

cleanup:
  free(db);
  free(sums);
  free(smooth);
}
