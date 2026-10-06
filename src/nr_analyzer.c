// by claude
#include "nr_analyzer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nw_fft.h"

#define NR_PI 3.14159265358979323846

/// Everything runs at about this rate, plenty for kicks and for the pitch
/// range a key is read from.
#define NR_TARGET_RATE 11025

#define NR_ONSET_FFT 512
#define NR_ONSET_HOP 128
/// Bins under this carry the kick, which decides where a beat sits.
#define NR_LOW_HZ 200.0
#define NR_LOG_GAIN 100.0f
/// The flux of a sharp attack peaks this many frames before the attack itself,
/// as the window starts catching it early.
#define NR_ONSET_LAG_FRAMES 0.25
/// Novelty is measured against the average flux this far around each frame.
#define NR_NOVELTY_SECONDS 0.25

#define NR_CHROMA_FFT 8192
#define NR_CHROMA_HOP 4096
#define NR_CHROMA_MIN_HZ 50.0
#define NR_CHROMA_MAX_HZ 2000.0
/// A spectral peak this far under the loudest one of its frame is left out.
#define NR_CHROMA_PEAK_FLOOR 0.001f
/// Frames whose loudest peak stays under this hold no pitch worth reading.
#define NR_CHROMA_SILENCE 1e-4f
/// Pitch is kept at this many bins per semitone until the tuning of the track
/// is known, recordings off concert pitch then still land on their notes.
#define NR_TUNING_BINS 10
#define NR_PITCH_BINS (12 * NR_TUNING_BINS)

#define NR_MIN_BPM 60.0
#define NR_MAX_BPM 200.0
#define NR_BPM_STEP 0.05
/// Center and width, in octaves, of the prior that settles between a tempo
/// and its half or double.
#define NR_PRIOR_BPM 120.0
#define NR_PRIOR_OCTAVES 0.9
#define NR_HARMONICS 4
/// A tempo hint is followed when the audio has a pulse this close to it,
/// scoring at least this share of the best one.
#define NR_HINT_REACH 0.04
#define NR_HINT_MIN_SHARE 0.4

#define NR_PHASE_BINS 72
/// The grid is fitted within this relative reach of the coarse tempo, then
/// again within one step of the best fit.
#define NR_FIT_REACH 0.006
#define NR_FIT_STEPS 60
#define NR_FINEST_STEPS 30

/// The grid is checked against every run of this many beats, and a run with a
/// pulse has to put its beats this close to the grid.
#define NR_SEGMENT_BEATS 32
#define NR_SEGMENT_MIN_RATIO 1.5
#define NR_SEGMENT_TOLERANCE_MS 15.0
/// The fold of a track with no pulse peaks this far over its average.
#define NR_PULSE_FLOOR 1.2
#define NR_PULSE_RANGE 1.0

/// Audio this far under the average power of the track counts as silence,
/// low enough that a quiet intro still counts as audio.
#define NR_SILENCE_DB -45.0

/// Loudness is followed over windows this long when looking for the fade out.
#define NR_LOUDNESS_SECONDS 1.0
/// The usual loudness of a track: this share of its audible windows plays
/// quieter.
#define NR_USUAL_LOUDNESS_SHARE 0.6
/// Whatever ends this far under the usual loudness fades out, and the fade
/// starts where the track last played within NR_FADE_OUT_START_DB of it.
#define NR_FADE_OUT_DB -9.0
#define NR_FADE_OUT_START_DB -3.0

#define NR_MIN_SECONDS 10.0

static const double kMajorProfile[12] = {0.748, 0.060, 0.488, 0.082, 0.670, 0.460, 0.096, 0.715, 0.104, 0.366, 0.057, 0.400};
static const double kMinorProfile[12] = {0.712, 0.084, 0.474, 0.618, 0.049, 0.460, 0.105, 0.747, 0.404, 0.067, 0.133, 0.330};

typedef struct {
  double b0, b1, b2, a1, a2;
  double z1, z2;
} NRBiquad;

struct NRAnalyzer {
  int decimation;
  /// Input samples left until the next one is kept.
  int countdown;
  NRBiquad lowpass[2];
  /// Rate everything after the decimation runs at.
  double rate;

  /// Samples not yet consumed, `samples[0]` is sample `base`.
  float* samples;
  int64_t count;
  int64_t capacity;
  int64_t base;
  int64_t total;
  int64_t next_onset;
  int64_t next_chroma;

  NWFft onset_fft;
  float* onset_window;
  float* previous_log;
  int low_bins;
  int has_previous;

  NWFft chroma_fft;
  float* chroma_window;
  float* magnitudes;
  int chroma_from;
  int chroma_to;
  double chroma_bin_hz;
  double pitch[NR_PITCH_BINS];

  /// One entry per onset frame.
  float* flux;
  float* low_flux;
  float* energy;
  int32_t frames;
  int32_t frame_capacity;
};

// ---------------------------------------------------------------------------
// input

/// Butterworth section by the bilinear transform.
static void nr_biquad_lowpass(NRBiquad* q, double sample_rate, double cutoff_hz, double quality) {
  const double w0 = 2.0 * NR_PI * cutoff_hz / sample_rate;
  const double cw = cos(w0);
  const double alpha = sin(w0) / (2.0 * quality);
  const double a0 = 1.0 + alpha;
  q->b0 = (1.0 - cw) * 0.5 / a0;
  q->b1 = (1.0 - cw) / a0;
  q->b2 = q->b0;
  q->a1 = -2.0 * cw / a0;
  q->a2 = (1.0 - alpha) / a0;
  q->z1 = 0.0;
  q->z2 = 0.0;
}

static inline double nr_biquad_run(NRBiquad* q, double x) {
  const double y = q->b0 * x + q->z1;
  q->z1 = q->b1 * x - q->a1 * y + q->z2;
  q->z2 = q->b2 * x - q->a2 * y;
  return y;
}

static int nr_samples_reserve(NRAnalyzer* a, int64_t needed) {
  if (needed <= a->capacity) return 1;
  int64_t capacity = a->capacity;
  while (capacity < needed) capacity *= 2;
  float* samples = (float*)realloc(a->samples, (size_t)capacity * sizeof(float));
  if (samples == NULL) return 0;
  a->samples = samples;
  a->capacity = capacity;
  return 1;
}

static int nr_frames_reserve(NRAnalyzer* a, int32_t needed) {
  if (needed <= a->frame_capacity) return 1;
  int32_t capacity = a->frame_capacity > 0 ? a->frame_capacity : 4096;
  while (capacity < needed) {
    if (capacity > INT32_MAX / 2) return 0;
    capacity *= 2;
  }
  float* flux = (float*)realloc(a->flux, (size_t)capacity * sizeof(float));
  if (flux == NULL) return 0;
  a->flux = flux;
  float* low_flux = (float*)realloc(a->low_flux, (size_t)capacity * sizeof(float));
  if (low_flux == NULL) return 0;
  a->low_flux = low_flux;
  float* energy = (float*)realloc(a->energy, (size_t)capacity * sizeof(float));
  if (energy == NULL) return 0;
  a->energy = energy;
  a->frame_capacity = capacity;
  return 1;
}

static void nr_hann(float* window, int size) {
  for (int i = 0; i < size; i++) window[i] = (float)(0.5 - 0.5 * cos(2.0 * NR_PI * i / size));
}

NRAnalyzer* nr_analyzer_create(int sample_rate) {
  if (sample_rate <= 0) return NULL;
  NRAnalyzer* a = (NRAnalyzer*)calloc(1, sizeof(NRAnalyzer));
  if (a == NULL) return NULL;

  int decimation = (int)((double)sample_rate / NR_TARGET_RATE + 0.5);
  if (decimation < 1) decimation = 1;
  a->decimation = decimation;
  a->countdown = decimation;
  a->rate = (double)sample_rate / decimation;
  if (decimation > 1) {
    // -- 4th order, its two sections share the cutoff and split the quality
    const double cutoff_hz = a->rate * 0.45;
    nr_biquad_lowpass(&a->lowpass[0], sample_rate, cutoff_hz, 0.54119610);
    nr_biquad_lowpass(&a->lowpass[1], sample_rate, cutoff_hz, 1.30656296);
  }

  a->onset_window = (float*)malloc(NR_ONSET_FFT * sizeof(float));
  a->previous_log = (float*)calloc(NR_ONSET_FFT / 2, sizeof(float));
  a->chroma_window = (float*)malloc(NR_CHROMA_FFT * sizeof(float));
  a->magnitudes = (float*)malloc((NR_CHROMA_FFT / 2) * sizeof(float));
  // Frames are centered on their timestamp, so the first ones start before
  // the audio does: the widest window's half is kept as leading silence.
  a->capacity = NR_CHROMA_FFT * 4;
  a->samples = (float*)calloc((size_t)a->capacity, sizeof(float));
  if (!nw_fft_init(&a->onset_fft, NR_ONSET_FFT) || !nw_fft_init(&a->chroma_fft, NR_CHROMA_FFT) || a->onset_window == NULL ||
      a->previous_log == NULL || a->chroma_window == NULL || a->magnitudes == NULL || a->samples == NULL) {
    nr_analyzer_free(a);
    return NULL;
  }
  nr_hann(a->onset_window, NR_ONSET_FFT);
  nr_hann(a->chroma_window, NR_CHROMA_FFT);
  a->count = NR_CHROMA_FFT / 2;
  a->base = -(NR_CHROMA_FFT / 2);

  a->low_bins = (int)(NR_LOW_HZ / (a->rate / NR_ONSET_FFT)) + 1;

  a->chroma_bin_hz = a->rate / NR_CHROMA_FFT;
  a->chroma_from = (int)ceil(NR_CHROMA_MIN_HZ / a->chroma_bin_hz);
  a->chroma_to = (int)(NR_CHROMA_MAX_HZ / a->chroma_bin_hz);
  if (a->chroma_from < 2) a->chroma_from = 2;
  if (a->chroma_to > NR_CHROMA_FFT / 2 - 2) a->chroma_to = NR_CHROMA_FFT / 2 - 2;
  return a;
}

void nr_analyzer_free(NRAnalyzer* a) {
  if (a == NULL) return;
  nw_fft_free(&a->onset_fft);
  nw_fft_free(&a->chroma_fft);
  free(a->onset_window);
  free(a->previous_log);
  free(a->chroma_window);
  free(a->magnitudes);
  free(a->samples);
  free(a->flux);
  free(a->low_flux);
  free(a->energy);
  free(a);
}

// ---------------------------------------------------------------------------
// frames

/// Spectral flux of the onset frame at `x`, over every bin and over the low
/// ones alone, plus the power of the hop it is centered on.
static int32_t nr_onset_frame(NRAnalyzer* a, const float* x) {
  if (!nr_frames_reserve(a, a->frames + 1)) return NW_ERR_ALLOC;
  NWFft* fft = &a->onset_fft;
  nw_fft_forward(fft, x, a->onset_window);

  // -- a full scale sine lands on `size / 4` once the window halved it
  const float magnitude_scale = 4.0f / NR_ONSET_FFT;
  const int half = NR_ONSET_FFT / 2;
  const int low_bins = a->low_bins;
  float* previous = a->previous_log;
  float flux = 0.0f;
  float low_flux = 0.0f;
  for (int k = 1; k < half; k++) {
    const float magnitude = sqrtf(nw_fft_bin_power(fft, k)) * magnitude_scale;
    const float level = log1pf(NR_LOG_GAIN * magnitude);
    const float rise = level - previous[k];
    if (rise > 0.0f) {
      flux += rise;
      if (k < low_bins) low_flux += rise;
    }
    previous[k] = level;
  }
  if (!a->has_previous) {
    flux = 0.0f;
    low_flux = 0.0f;
    a->has_previous = 1;
  }

  const float* hop = x + half;
  double power = 0.0;
  for (int i = 0; i < NR_ONSET_HOP; i++) power += (double)hop[i] * hop[i];

  a->flux[a->frames] = flux;
  a->low_flux[a->frames] = low_flux;
  a->energy[a->frames] = (float)(power / NR_ONSET_HOP);
  a->frames++;
  return NW_OK;
}

/// Adds the pitch of the chroma frame at `x` to the track's, every frame with
/// pitch in it weighing the same.
static void nr_chroma_frame(NRAnalyzer* a, const float* x) {
  NWFft* fft = &a->chroma_fft;
  nw_fft_forward(fft, x, a->chroma_window);

  const int from = a->chroma_from;
  const int to = a->chroma_to;
  float* magnitudes = a->magnitudes;
  float loudest = 0.0f;
  for (int k = from - 1; k <= to; k++) {
    const float magnitude = sqrtf(nw_fft_bin_power(fft, k));
    magnitudes[k] = magnitude;
    if (magnitude > loudest) loudest = magnitude;
  }
  const float magnitude_scale = 4.0f / NR_CHROMA_FFT;
  if (loudest * magnitude_scale < NR_CHROMA_SILENCE) return;

  const float peak_floor = loudest * NR_CHROMA_PEAK_FLOOR;
  double frame[NR_PITCH_BINS] = {0.0};
  double total = 0.0;
  for (int k = from; k < to; k++) {
    const float m = magnitudes[k];
    if (m <= peak_floor || m <= magnitudes[k - 1] || m < magnitudes[k + 1]) continue;

    // -- the true frequency of the peak, from the log magnitudes around it
    const double left = log(magnitudes[k - 1] + 1e-12);
    const double center = log(m + 1e-12);
    const double right = log(magnitudes[k + 1] + 1e-12);
    const double curve = left - 2.0 * center + right;
    const double offset = curve < 0.0 ? 0.5 * (left - right) / curve : 0.0;
    const double hz = (k + offset) * a->chroma_bin_hz;

    const double pitch = 12.0 * log2(hz / 440.0) + 69.0;
    const int bin = (int)floor(pitch * NR_TUNING_BINS + 0.5);
    frame[((bin % NR_PITCH_BINS) + NR_PITCH_BINS) % NR_PITCH_BINS] += m;
    total += m;
  }

  if (total <= 0.0) return;
  for (int i = 0; i < NR_PITCH_BINS; i++) a->pitch[i] += frame[i] / total;
}

/// Runs every frame the buffered samples cover whose center lies before
/// `limit`, then lets go of the samples no later frame reads.
static int32_t nr_drain(NRAnalyzer* a, int64_t limit) {
  int64_t onset_start;
  for (;;) {
    const int64_t center = a->next_onset * NR_ONSET_HOP;
    onset_start = center - NR_ONSET_FFT / 2;
    if (center >= limit || onset_start + NR_ONSET_FFT > a->base + a->count) break;
    const int32_t error = nr_onset_frame(a, a->samples + (onset_start - a->base));
    if (error != NW_OK) return error;
    a->next_onset++;
  }

  int64_t chroma_start;
  for (;;) {
    const int64_t center = a->next_chroma * NR_CHROMA_HOP;
    chroma_start = center - NR_CHROMA_FFT / 2;
    if (center >= limit || chroma_start + NR_CHROMA_FFT > a->base + a->count) break;
    nr_chroma_frame(a, a->samples + (chroma_start - a->base));
    a->next_chroma++;
  }

  const int64_t keep_from = onset_start < chroma_start ? onset_start : chroma_start;
  int64_t drop = keep_from - a->base;
  if (drop > a->count) drop = a->count;
  if (drop > 0) {
    const int64_t kept = a->count - drop;
    if (kept > 0) memmove(a->samples, a->samples + drop, (size_t)kept * sizeof(float));
    a->count = kept;
    a->base += drop;
  }
  return NW_OK;
}

int32_t nr_analyzer_feed(NRAnalyzer* a, const float* samples, int count) {
  const int decimation = a->decimation;
  if (!nr_samples_reserve(a, a->count + count / decimation + 1)) return NW_ERR_ALLOC;
  float* dst = a->samples + a->count;
  int64_t kept = 0;
  if (decimation == 1) {
    memcpy(dst, samples, (size_t)count * sizeof(float));
    kept = count;
  } else {
    NRBiquad* first = &a->lowpass[0];
    NRBiquad* second = &a->lowpass[1];
    int countdown = a->countdown;
    for (int i = 0; i < count; i++) {
      const double y = nr_biquad_run(second, nr_biquad_run(first, samples[i]));
      if (--countdown == 0) {
        countdown = decimation;
        dst[kept++] = (float)y;
      }
    }
    a->countdown = countdown;
  }
  a->count += kept;
  a->total += kept;
  return nr_drain(a, INT64_MAX);
}

// ---------------------------------------------------------------------------
// analysis

/// How much `flux` rises over its own average around every frame.
static void nr_novelty(const float* flux, int32_t frames, int reach, float* out, double* sums) {
  sums[0] = 0.0;
  for (int32_t t = 0; t < frames; t++) sums[t + 1] = sums[t] + flux[t];
  for (int32_t t = 0; t < frames; t++) {
    const int32_t from = t - reach < 0 ? 0 : t - reach;
    const int32_t to = t + reach + 1 > frames ? frames : t + reach + 1;
    const double around = (sums[to] - sums[from]) / (to - from);
    const double rise = flux[t] - around;
    out[t] = rise > 0.0 ? (float)rise : 0.0f;
  }
}

static double nr_mean(const float* values, int32_t count) {
  double sum = 0.0;
  for (int32_t i = 0; i < count; i++) sum += values[i];
  return count > 0 ? sum / count : 0.0;
}

static inline double nr_interpolate(const double* values, int count, double position) {
  const int i = (int)position;
  if (i + 1 >= count) return values[count - 1];
  const double f = position - i;
  return values[i] * (1.0 - f) + values[i + 1] * f;
}

/// Autocorrelation of the novelty, so each lag tells how alike the track is
/// with itself that many frames later.
static void nr_autocorrelate(const float* novelty, int32_t frames, double* correlation, int lags) {
  for (int lag = 0; lag < lags; lag++) {
    double sum = 0.0;
    const int32_t end = frames - lag;
    for (int32_t t = 0; t < end; t++) sum += (double)novelty[t] * novelty[t + lag];
    correlation[lag] = end > 0 ? sum / end : 0.0;
  }
}

/// How strongly the track pulses at `bpm`, counting its first harmonics too.
static double nr_salience(const double* correlation, int lags, double fps, double bpm) {
  const double period = 60.0 * fps / bpm;
  double salience = 0.0;
  for (int h = 1; h <= NR_HARMONICS; h++) {
    const double lag = period * h;
    if (lag >= lags - 1) break;
    salience += nr_interpolate(correlation, lags, lag);
  }
  return salience;
}

static double nr_prior(double bpm) {
  const double octaves = log2(bpm / NR_PRIOR_BPM) / NR_PRIOR_OCTAVES;
  return exp(-0.5 * octaves * octaves);
}

/// Best tempo between `low` and `high`, refined between the steps around it.
/// `weighted` applies the prior.
static double nr_best_tempo(const double* correlation, int lags, double fps, double low, double high, int weighted, double* score) {
  double best_bpm = 0.0;
  double best = -1.0;
  for (double bpm = low; bpm <= high; bpm += NR_BPM_STEP) {
    double s = nr_salience(correlation, lags, fps, bpm);
    if (weighted) s *= nr_prior(bpm);
    if (s > best) {
      best = s;
      best_bpm = bpm;
    }
  }
  if (best_bpm > 0.0) {
    const double before = nr_salience(correlation, lags, fps, best_bpm - NR_BPM_STEP) * (weighted ? nr_prior(best_bpm - NR_BPM_STEP) : 1.0);
    const double after = nr_salience(correlation, lags, fps, best_bpm + NR_BPM_STEP) * (weighted ? nr_prior(best_bpm + NR_BPM_STEP) : 1.0);
    const double curve = before - 2.0 * best + after;
    if (curve < 0.0) best_bpm += 0.5 * (before - after) / curve * NR_BPM_STEP;
  }
  *score = best;
  return best_bpm;
}

/// Folds `envelope[from, to)` onto one beat of `period` frames, as a circular
/// histogram of `NR_PHASE_BINS` bins.
static void nr_fold(const float* envelope, int32_t from, int32_t to, double period, double* histogram) {
  memset(histogram, 0, NR_PHASE_BINS * sizeof(double));
  const double step = NR_PHASE_BINS / period;
  double position = fmod(from * step, NR_PHASE_BINS);
  for (int32_t t = from; t < to; t++) {
    const float value = envelope[t];
    if (value > 0.0f) {
      const int i = (int)position;
      const double f = position - i;
      histogram[i] += value * (1.0 - f);
      histogram[i + 1 == NR_PHASE_BINS ? 0 : i + 1] += value * f;
    }
    position += step;
    if (position >= NR_PHASE_BINS) position -= NR_PHASE_BINS;
  }
}

/// Peak of a folded beat: its height, where it sits in bins, and how far it
/// stands over the average bin.
static double nr_fold_peak(const double* histogram, double* phase_bins, double* ratio) {
  double smooth[NR_PHASE_BINS];
  double sum = 0.0;
  int peak = 0;
  for (int i = 0; i < NR_PHASE_BINS; i++) {
    const double before = histogram[i == 0 ? NR_PHASE_BINS - 1 : i - 1];
    const double after = histogram[i + 1 == NR_PHASE_BINS ? 0 : i + 1];
    smooth[i] = 0.25 * before + 0.5 * histogram[i] + 0.25 * after;
    sum += smooth[i];
    if (smooth[i] > smooth[peak]) peak = i;
  }
  const double before = smooth[peak == 0 ? NR_PHASE_BINS - 1 : peak - 1];
  const double after = smooth[peak + 1 == NR_PHASE_BINS ? 0 : peak + 1];
  const double curve = before - 2.0 * smooth[peak] + after;
  double position = peak + (curve < 0.0 ? 0.5 * (before - after) / curve : 0.0);
  if (position < 0.0) position += NR_PHASE_BINS;
  if (position >= NR_PHASE_BINS) position -= NR_PHASE_BINS;
  *phase_bins = position;
  const double average = sum / NR_PHASE_BINS;
  *ratio = average > 0.0 ? smooth[peak] / average : 0.0;
  return smooth[peak];
}

/// Period, in frames, whose fold of the whole track peaks highest, searched
/// within `reach` of `period` in `steps` steps each way.
static double nr_fit_period(const float* envelope, int32_t frames, double period, double reach, int steps) {
  double histogram[NR_PHASE_BINS];
  double best_period = period;
  double best = -1.0;
  for (int i = -steps; i <= steps; i++) {
    const double candidate = period * (1.0 + reach * i / steps);
    nr_fold(envelope, 0, frames, candidate, histogram);
    double phase_bins, ratio;
    const double peak = nr_fold_peak(histogram, &phase_bins, &ratio);
    if (peak > best) {
      best = peak;
      best_period = candidate;
    }
  }
  return best_period;
}

static double nr_circular_distance(double a, double b, double cycle) {
  double d = fmod(a - b, cycle);
  if (d < -cycle * 0.5) d += cycle;
  if (d >= cycle * 0.5) d -= cycle;
  return d;
}

/// Share of the pulsing runs of the track that keep their beats on the grid,
/// halved when the first or the last of them drifts off it, since those are
/// where tracks get mixed.
static double nr_grid_consistency(const float* envelope, int32_t frames, double period, double phase_bins, double ms_per_frame) {
  const int32_t length = (int32_t)(period * NR_SEGMENT_BEATS);
  if (length <= 0 || length > frames) return 1.0;

  double histogram[NR_PHASE_BINS];
  double total = 0.0;
  double consistent = 0.0;
  int first_consistent = -1;
  int last_consistent = -1;
  int32_t from = 0;
  for (;;) {
    int32_t to = from + length;
    if (to > frames) {
      from = frames - length;
      to = frames;
    }
    nr_fold(envelope, from, to, period, histogram);
    double run_phase, ratio;
    nr_fold_peak(histogram, &run_phase, &ratio);
    if (ratio >= NR_SEGMENT_MIN_RATIO) {
      double strength = 0.0;
      for (int32_t t = from; t < to; t++) strength += envelope[t];
      const double off_bins = nr_circular_distance(run_phase, phase_bins, NR_PHASE_BINS);
      const double off_ms = off_bins / NR_PHASE_BINS * period * ms_per_frame;
      const int fits = fabs(off_ms) <= NR_SEGMENT_TOLERANCE_MS;
      total += strength;
      if (fits) consistent += strength;
      if (first_consistent < 0) first_consistent = fits;
      last_consistent = fits;
    }
    if (to == frames) break;
    from = to;
  }
  if (total <= 0.0) return 0.0;
  double share = consistent / total;
  if (first_consistent == 0 || last_consistent == 0) share *= 0.5;
  return share;
}

static double nr_correlate(const double* chroma, const double* profile, int tonic) {
  double chroma_mean = 0.0, profile_mean = 0.0;
  for (int i = 0; i < 12; i++) {
    chroma_mean += chroma[(tonic + i) % 12];
    profile_mean += profile[i];
  }
  chroma_mean /= 12.0;
  profile_mean /= 12.0;
  double covariance = 0.0, chroma_variance = 0.0, profile_variance = 0.0;
  for (int i = 0; i < 12; i++) {
    const double c = chroma[(tonic + i) % 12] - chroma_mean;
    const double p = profile[i] - profile_mean;
    covariance += c * p;
    chroma_variance += c * c;
    profile_variance += p * p;
  }
  const double denominator = sqrt(chroma_variance * profile_variance);
  return denominator > 0.0 ? covariance / denominator : 0.0;
}

/// Folds the pitch of the track into 12 pitch classes, centered on where its
/// notes actually sit.
static void nr_tuned_chroma(const NRAnalyzer* a, double* chroma) {
  // -- the circular mean of where pitch falls within a semitone
  double x = 0.0, y = 0.0;
  for (int i = 0; i < NR_PITCH_BINS; i++) {
    const double angle = 2.0 * NR_PI * (i % NR_TUNING_BINS) / NR_TUNING_BINS;
    x += a->pitch[i] * cos(angle);
    y += a->pitch[i] * sin(angle);
  }
  const double tuning = atan2(y, x) / (2.0 * NR_PI);

  memset(chroma, 0, 12 * sizeof(double));
  for (int i = 0; i < NR_PITCH_BINS; i++) {
    const double semitones = (double)i / NR_TUNING_BINS - tuning;
    const double nearest = floor(semitones + 0.5);
    const double weight = 1.0 - 2.0 * fabs(semitones - nearest);
    const int pitch_class = (((int)nearest % 12) + 12) % 12;
    chroma[pitch_class] += a->pitch[i] * weight;
  }
}

static void nr_find_key(const NRAnalyzer* a, NRResult* result) {
  result->key = -1;
  result->key_confidence = 0.0f;
  double chroma[12];
  nr_tuned_chroma(a, chroma);
  double best = 0.0;
  for (int tonic = 0; tonic < 12; tonic++) {
    const double major = nr_correlate(chroma, kMajorProfile, tonic);
    const double minor = nr_correlate(chroma, kMinorProfile, tonic);
    if (major > best) {
      best = major;
      result->key = tonic;
    }
    if (minor > best) {
      best = minor;
      result->key = 12 + tonic;
    }
  }
  result->key_confidence = (float)best;
}

static int nr_compare_doubles(const void* a, const void* b) {
  const double x = *(const double*)a;
  const double y = *(const double*)b;
  return (x > y) - (x < y);
}

/// Average power of the window centered on frame `t`, kept within the
/// audible range.
static inline double nr_window_level(const double* sums, int32_t t, int32_t window, int32_t first, int32_t last) {
  int32_t from = t - window / 2;
  int32_t to = t + window / 2 + 1;
  if (from < first) from = first;
  if (to > last + 1) to = last + 1;
  return (sums[to] - sums[from]) / (to - from);
}

/// Where the audio rises above and last falls below silence, and where it
/// settles into its fade out or quiet outro.
static int32_t nr_find_audible_range(const NRAnalyzer* a, double ms_per_frame, NRResult* result) {
  const int32_t frames = a->frames;
  const float* energy = a->energy;
  const double silence = nr_mean(energy, frames) * pow(10.0, NR_SILENCE_DB / 10.0);
  int32_t first = -1;
  int32_t last = -1;
  for (int32_t t = 0; t < frames; t++) {
    if (energy[t] > silence) {
      if (first < 0) first = t;
      last = t;
    }
  }
  if (first < 0 || silence <= 0.0) return NW_OK;
  result->audible_start_ms = (int64_t)(first * ms_per_frame);
  result->audible_end_ms = (int64_t)((last + 1) * ms_per_frame);
  result->fade_out_start_ms = result->audible_end_ms;

  const int32_t window = (int32_t)(NR_LOUDNESS_SECONDS * 1000.0 / ms_per_frame + 0.5);
  if (window < 4 || last - first < window * 4) return NW_OK;

  double* sums = (double*)malloc(((size_t)frames + 1) * sizeof(double));
  const int32_t step = window / 4;
  const int32_t count = (last - first) / step + 1;
  double* levels = (double*)malloc((size_t)count * sizeof(double));
  if (sums == NULL || levels == NULL) {
    free(sums);
    free(levels);
    return NW_ERR_ALLOC;
  }
  sums[0] = 0.0;
  for (int32_t t = 0; t < frames; t++) sums[t + 1] = sums[t] + energy[t];

  int32_t sampled = 0;
  for (int32_t t = first; t <= last && sampled < count; t += step) levels[sampled++] = nr_window_level(sums, t, window, first, last);
  qsort(levels, (size_t)sampled, sizeof(double), nr_compare_doubles);
  const double usual = levels[(int32_t)(sampled * NR_USUAL_LOUDNESS_SHARE)];
  free(levels);

  const double fade_floor = usual * pow(10.0, NR_FADE_OUT_DB / 10.0);
  const double fade_start_level = usual * pow(10.0, NR_FADE_OUT_START_DB / 10.0);
  int32_t t = last;
  while (t > first && nr_window_level(sums, t, window, first, last) < fade_floor) t--;
  while (t > first && nr_window_level(sums, t, window, first, last) < fade_start_level) t--;
  free(sums);
  result->fade_out_start_ms = (int64_t)(t * ms_per_frame);
  return NW_OK;
}

/// Tempo and beat grid from the onset frames.
static int32_t nr_find_beat(const NRAnalyzer* a, double bpm_hint, double ms_per_frame, NRResult* result) {
  const int32_t frames = a->frames;
  const double fps = 1000.0 / ms_per_frame;
  if (frames < fps * NR_MIN_SECONDS) return NW_OK;

  const int lags = (int)ceil(NR_HARMONICS * 60.0 * fps / NR_MIN_BPM) + 2;
  if (lags >= frames) return NW_OK;

  float* novelty = (float*)malloc((size_t)frames * sizeof(float));
  float* low_novelty = (float*)malloc((size_t)frames * sizeof(float));
  double* sums = (double*)malloc(((size_t)frames + 1) * sizeof(double));
  double* correlation = (double*)malloc((size_t)lags * sizeof(double));
  int32_t error = NW_OK;
  if (novelty == NULL || low_novelty == NULL || sums == NULL || correlation == NULL) {
    error = NW_ERR_ALLOC;
    goto cleanup;
  }

  const int reach = (int)(fps * NR_NOVELTY_SECONDS + 0.5);
  nr_novelty(a->flux, frames, reach, novelty, sums);
  nr_novelty(a->low_flux, frames, reach, low_novelty, sums);
  nr_autocorrelate(novelty, frames, correlation, lags);

  double best_score;
  double bpm = nr_best_tempo(correlation, lags, fps, NR_MIN_BPM, NR_MAX_BPM, 1, &best_score);
  if (bpm <= 0.0 || best_score <= 0.0) goto cleanup;

  if (bpm_hint > 0.0) {
    double hint_score;
    const double hinted = nr_best_tempo(correlation, lags, fps, bpm_hint * (1.0 - NR_HINT_REACH), bpm_hint * (1.0 + NR_HINT_REACH), 0, &hint_score);
    const double unweighted_best = nr_salience(correlation, lags, fps, bpm);
    if (hinted > 0.0 && hint_score >= unweighted_best * NR_HINT_MIN_SHARE) bpm = hinted;
  }

  // -- the kick says where the beat sits, the rest only backs it up
  const double novelty_mean = nr_mean(novelty, frames);
  const double low_mean = nr_mean(low_novelty, frames);
  for (int32_t t = 0; t < frames; t++) {
    const double low = low_mean > 0.0 ? low_novelty[t] / low_mean : 0.0;
    const double all = novelty_mean > 0.0 ? novelty[t] / novelty_mean : 0.0;
    low_novelty[t] = (float)(low + 0.5 * all);
  }
  const float* envelope = low_novelty;

  double period = 60.0 * fps / bpm;
  period = nr_fit_period(envelope, frames, period, NR_FIT_REACH, NR_FIT_STEPS);
  period = nr_fit_period(envelope, frames, period, NR_FIT_REACH / NR_FIT_STEPS, NR_FINEST_STEPS);

  double histogram[NR_PHASE_BINS];
  nr_fold(envelope, 0, frames, period, histogram);
  double phase_bins, ratio;
  nr_fold_peak(histogram, &phase_bins, &ratio);

  double offset_ms = (phase_bins / NR_PHASE_BINS * period + NR_ONSET_LAG_FRAMES) * ms_per_frame;
  const double period_ms = period * ms_per_frame;
  offset_ms = fmod(offset_ms, period_ms);
  if (offset_ms < 0.0) offset_ms += period_ms;

  double pulse = (ratio - NR_PULSE_FLOOR) / NR_PULSE_RANGE;
  if (pulse < 0.0) pulse = 0.0;
  if (pulse > 1.0) pulse = 1.0;
  const double consistency = nr_grid_consistency(envelope, frames, period, phase_bins, ms_per_frame);

  result->bpm = 60.0 * fps / period;
  result->beat_offset_ms = offset_ms;
  result->beat_confidence = (float)(pulse * consistency);

cleanup:
  free(novelty);
  free(low_novelty);
  free(sums);
  free(correlation);
  return error;
}

int32_t nr_analyzer_finish(NRAnalyzer* a, double bpm_hint, NRResult* result) {
  result->bpm = 0.0;
  result->beat_offset_ms = 0.0;
  result->beat_confidence = 0.0f;
  result->key = -1;
  result->key_confidence = 0.0f;
  result->audible_start_ms = 0;
  result->audible_end_ms = 0;
  result->fade_out_start_ms = 0;
  if (a->total == 0) return NW_ERR_NO_OUTPUT;

  // The last frames reach past the end of the audio, into silence.
  const int pad = NR_CHROMA_FFT / 2;
  if (!nr_samples_reserve(a, a->count + pad)) return NW_ERR_ALLOC;
  memset(a->samples + a->count, 0, (size_t)pad * sizeof(float));
  a->count += pad;
  const int32_t error = nr_drain(a, a->total);
  if (error != NW_OK) return error;
  if (a->frames == 0) return NW_ERR_NO_OUTPUT;

  const double ms_per_frame = NR_ONSET_HOP * 1000.0 / a->rate;
  const int32_t range_error = nr_find_audible_range(a, ms_per_frame, result);
  if (range_error != NW_OK) return range_error;
  nr_find_key(a, result);
  return nr_find_beat(a, bpm_hint, ms_per_frame, result);
}
