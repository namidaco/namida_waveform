// by claude
#include "namida_spectrum.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nw_decoder.h"
#include "nw_fft.h"

#define NS_VERSION 1

#define NS_MIN_FPS 1
#define NS_MAX_FPS 120
#define NS_DEFAULT_FPS 30

#define NS_MIN_BANDS 1
#define NS_MAX_BANDS 64
#define NS_DEFAULT_BANDS 16

/// The transform spans the power of two covering this much audio, long enough
/// to tell the lowest bands apart, short enough to still follow a beat.
#define NS_WINDOW_SECONDS 0.04
#define NS_MIN_FFT 256
#define NS_MAX_FFT 16384

#define NS_MIN_HZ 40.0
#define NS_MAX_HZ 16000.0

/// Rows are first written on a fixed scale reaching this far below full scale,
/// so a track of any length costs no more memory than its final rows do.
#define NS_FLOOR_DB -90.0

/// How far below the loudest band of the track a level can sit before it
/// reads as nothing.
#define NS_RANGE_DB 54.0

/// The share of frames allowed to clip, so one stray peak does not dim a
/// whole track.
#define NS_CLIPPED_FRAMES 0.01

/// Bands under this carry the beat, the rest only hint at it.
#define NS_BEAT_HZ 250.0
#define NS_BEAT_OTHER_WEIGHT 0.3

/// A beat has to stand this far above the activity around it.
#define NS_BEAT_THRESHOLD 1.5
#define NS_BEAT_WINDOW_SECONDS 0.5

#define NS_PI 3.14159265358979323846

typedef struct {
  int sample_rate;
  int fps;
  int band_count;
  int stride;

  /// Samples one transform reads, and the size of the complex transform it
  /// is folded into.
  int fft_size;
  int half;

  float* window;
  NWFft fft;

  /// `band_count + 1` bins, band `b` sums `[edges[b], edges[b + 1])`.
  int* edges;
  float* beat_weights;
  double power_scale;

  /// Mono samples not yet consumed, `samples[0]` is sample `base` of the track.
  float* samples;
  int64_t count;
  int64_t capacity;
  int64_t base;
  int64_t total;
  int64_t next_frame;

  uint8_t* rows;
  int32_t row_count;
  int32_t row_capacity;
} NSState;

static void ns_state_free(NSState* s) {
  free(s->window);
  nw_fft_free(&s->fft);
  free(s->edges);
  free(s->beat_weights);
  free(s->samples);
}

static int ns_state_prepare(NSState* s, int sample_rate) {
  s->sample_rate = sample_rate;

  int fft_size = NS_MIN_FFT;
  while (fft_size < NS_MAX_FFT && fft_size < sample_rate * NS_WINDOW_SECONDS) fft_size <<= 1;
  const int half = fft_size / 2;
  s->fft_size = fft_size;
  s->half = half;

  s->window = (float*)malloc((size_t)fft_size * sizeof(float));
  s->edges = (int*)malloc((size_t)(s->band_count + 1) * sizeof(int));
  s->beat_weights = (float*)malloc((size_t)s->band_count * sizeof(float));
  if (!nw_fft_init(&s->fft, fft_size) || s->window == NULL || s->edges == NULL || s->beat_weights == NULL) {
    return 0;
  }

  for (int i = 0; i < fft_size; i++) {
    s->window[i] = (float)(0.5 - 0.5 * cos(2.0 * NS_PI * i / fft_size));
  }

  // A full scale sine lands on `fft_size / 4` once the window halved it.
  const double full_scale = fft_size / 4.0;
  s->power_scale = 1.0 / (full_scale * full_scale);

  const double bin_hz = (double)sample_rate / fft_size;
  const double low_hz = NS_MIN_HZ;
  double high_hz = sample_rate * 0.45;
  if (high_hz > NS_MAX_HZ) high_hz = NS_MAX_HZ;
  if (high_hz < low_hz * 2.0) high_hz = low_hz * 2.0;
  const double ratio = high_hz / low_hz;

  int previous = 0;
  for (int i = 0; i <= s->band_count; i++) {
    const double hz = low_hz * pow(ratio, (double)i / s->band_count);
    int bin = (int)(hz / bin_hz + 0.5);
    // every band keeps a bin of its own, the lowest ones are narrower than one
    if (bin <= previous) bin = previous + 1;
    if (bin > half) bin = half;
    s->edges[i] = bin;
    previous = bin;
  }
  for (int b = 0; b < s->band_count; b++) {
    const double center_hz = (s->edges[b] + s->edges[b + 1]) * 0.5 * bin_hz;
    s->beat_weights[b] = center_hz < NS_BEAT_HZ ? 1.0f : (float)NS_BEAT_OTHER_WEIGHT;
  }

  // Frames are centered on their timestamp, so the first one starts half a
  // window before the audio does.
  s->capacity = (int64_t)fft_size * 4;
  s->samples = (float*)malloc((size_t)s->capacity * sizeof(float));
  if (s->samples == NULL) return 0;
  memset(s->samples, 0, (size_t)half * sizeof(float));
  s->count = half;
  s->base = -half;
  return 1;
}

static int ns_samples_reserve(NSState* s, int64_t needed) {
  if (needed <= s->capacity) return 1;
  int64_t capacity = s->capacity;
  while (capacity < needed) capacity *= 2;
  float* samples = (float*)realloc(s->samples, (size_t)capacity * sizeof(float));
  if (samples == NULL) return 0;
  s->samples = samples;
  s->capacity = capacity;
  return 1;
}

static int ns_rows_reserve(NSState* s, int32_t needed) {
  if (needed <= s->row_capacity) return 1;
  int32_t capacity = s->row_capacity > 0 ? s->row_capacity : 1024;
  while (capacity < needed) {
    if (capacity > (INT32_MAX / 2) / s->stride) return 0;
    capacity *= 2;
  }
  uint8_t* rows = (uint8_t*)realloc(s->rows, (size_t)capacity * s->stride);
  if (rows == NULL) return 0;
  s->rows = rows;
  s->row_capacity = capacity;
  return 1;
}

static int32_t ns_append(NSState* s, const AVFrame* frame) {
  const int n = frame->nb_samples;
  if (!ns_samples_reserve(s, s->count + n)) return NW_ERR_ALLOC;
  const int32_t error = nw_decoder_downmix(frame, s->samples + s->count);
  if (error != NW_OK) return error;

  s->count += n;
  s->total += n;
  return NW_OK;
}

/// Transforms the `fft_size` samples at `x` and writes the level of every
/// band to `row`.
static void ns_transform(NSState* s, const float* x, uint8_t* row) {
  NWFft* fft = &s->fft;
  nw_fft_forward(fft, x, s->window);

  const double power_scale = s->power_scale;
  const int band_count = s->band_count;

  for (int b = 0; b < band_count; b++) {
    const int from = s->edges[b];
    const int to = s->edges[b + 1];
    float power = 0.0f;
    for (int k = from; k < to; k++) power += nw_fft_bin_power(fft, k);
    const double db = 10.0 * log10((double)power * power_scale + 1e-20);
    const double level = (db - NS_FLOOR_DB) * (255.0 / -NS_FLOOR_DB);
    row[b] = level <= 0.0 ? 0 : (level >= 255.0 ? 255 : (uint8_t)(level + 0.5));
  }
  row[band_count] = 0;
}

/// Writes a row for every frame the buffered samples cover, up to the frame
/// centered on `limit`, then lets go of the samples no later frame reads.
static int32_t ns_drain(NSState* s, int64_t limit) {
  const int fft_size = s->fft_size;
  const int half = s->half;
  int64_t start;
  for (;;) {
    const int64_t center = s->next_frame * s->sample_rate / s->fps;
    start = center - half;
    if (center >= limit) break;
    if (start + fft_size > s->base + s->count) break;

    if (!ns_rows_reserve(s, s->row_count + 1)) return NW_ERR_ALLOC;
    uint8_t* row = s->rows + (size_t)s->row_count * s->stride;
    ns_transform(s, s->samples + (start - s->base), row);
    s->row_count++;
    s->next_frame++;
  }

  int64_t drop = start - s->base;
  if (drop > s->count) drop = s->count;
  if (drop > 0) {
    const int64_t kept = s->count - drop;
    if (kept > 0) memmove(s->samples, s->samples + drop, (size_t)kept * sizeof(float));
    s->count = kept;
    s->base += drop;
  }
  return NW_OK;
}

/// Moves every level from the fixed scale onto one relative to the loudest
/// band of the track, then fills in the beat strength of every row.
static int32_t ns_finish(NSState* s) {
  const int32_t row_count = s->row_count;
  const int band_count = s->band_count;
  const int stride = s->stride;
  if (row_count == 0) return NW_OK;

  int64_t histogram[256] = {0};
  for (int32_t t = 0; t < row_count; t++) {
    const uint8_t* row = s->rows + (size_t)t * stride;
    uint8_t loudest = 0;
    for (int b = 0; b < band_count; b++) {
      if (row[b] > loudest) loudest = row[b];
    }
    histogram[loudest]++;
  }

  int64_t allowed = (int64_t)((row_count - histogram[0]) * NS_CLIPPED_FRAMES);
  int top = 255;
  while (top > 0 && histogram[top] <= allowed) {
    allowed -= histogram[top];
    top--;
  }

  // -- a track quieter than the range itself only has what is above silence to show
  double bottom = top - NS_RANGE_DB * (255.0 / -NS_FLOOR_DB);
  if (bottom < 0.0) bottom = 0.0;
  const double range = top - bottom;
  uint8_t remap[256];
  for (int v = 0; v < 256; v++) {
    const double level = range > 0.0 ? (v - bottom) * (255.0 / range) : 0.0;
    remap[v] = level <= 0.0 ? 0 : (level >= 255.0 ? 255 : (uint8_t)(level + 0.5));
  }
  for (int32_t t = 0; t < row_count; t++) {
    uint8_t* row = s->rows + (size_t)t * stride;
    for (int b = 0; b < band_count; b++) row[b] = remap[row[b]];
  }

  // -- how much every band rose since the row before, summed up to each row
  double* rise = (double*)malloc(((size_t)row_count + 1) * sizeof(double));
  if (rise == NULL) return NW_ERR_ALLOC;
  rise[0] = 0.0;
  rise[1] = 0.0;
  for (int32_t t = 1; t < row_count; t++) {
    const uint8_t* row = s->rows + (size_t)t * stride;
    const uint8_t* previous = row - stride;
    float risen = 0.0f;
    for (int b = 0; b < band_count; b++) {
      const int delta = (int)row[b] - (int)previous[b];
      if (delta > 0) risen += delta * s->beat_weights[b];
    }
    rise[t + 1] = rise[t] + risen;
  }

  int32_t reach = (int32_t)(s->fps * NS_BEAT_WINDOW_SECONDS);
  if (reach < 1) reach = 1;

  float* beats = (float*)malloc((size_t)row_count * sizeof(float));
  if (beats == NULL) {
    free(rise);
    return NW_ERR_ALLOC;
  }
  float strongest = 0.0f;
  for (int32_t t = 0; t < row_count; t++) {
    const int32_t from = t - reach < 0 ? 0 : t - reach;
    const int32_t to = t + reach + 1 > row_count ? row_count : t + reach + 1;
    const double around = (rise[to] - rise[from]) / (to - from);
    const double risen = rise[t + 1] - rise[t];
    const double beat = risen - around * NS_BEAT_THRESHOLD;
    beats[t] = beat > 0.0 ? (float)beat : 0.0f;
    if (beats[t] > strongest) strongest = beats[t];
  }
  free(rise);

  if (strongest > 0.0f) {
    // -- the same share of stray peaks is let to clip here
    int64_t beat_histogram[256] = {0};
    for (int32_t t = 0; t < row_count; t++) {
      beat_histogram[(int)(beats[t] / strongest * 255.0f)]++;
    }
    int64_t beat_allowed = (int64_t)((row_count - beat_histogram[0]) * NS_CLIPPED_FRAMES);
    int beat_top = 255;
    while (beat_top > 1 && beat_histogram[beat_top] <= beat_allowed) {
      beat_allowed -= beat_histogram[beat_top];
      beat_top--;
    }
    const float ceiling = strongest * beat_top / 255.0f;
    for (int32_t t = 0; t < row_count; t++) {
      const float level = beats[t] / ceiling * 255.0f;
      s->rows[(size_t)t * stride + band_count] = level >= 255.0f ? 255 : (uint8_t)(level + 0.5f);
    }
  }
  free(beats);
  return NW_OK;
}

NW_EXPORT int32_t ns_version(void) { return NS_VERSION; }

NW_EXPORT void ns_result_free(NSResult* result) {
  if (result == NULL) return;
  free(result->data);
  free(result);
}

NW_EXPORT NSResult* ns_extract(const char* path, int32_t frames_per_second, int32_t band_count) {
  NSResult* result = (NSResult*)calloc(1, sizeof(NSResult));
  if (result == NULL) return NULL;

  if (frames_per_second <= 0) frames_per_second = NS_DEFAULT_FPS;
  if (frames_per_second < NS_MIN_FPS) frames_per_second = NS_MIN_FPS;
  if (frames_per_second > NS_MAX_FPS) frames_per_second = NS_MAX_FPS;
  if (band_count <= 0) band_count = NS_DEFAULT_BANDS;
  if (band_count < NS_MIN_BANDS) band_count = NS_MIN_BANDS;
  if (band_count > NS_MAX_BANDS) band_count = NS_MAX_BANDS;

  NWDecoder decoder;
  NSState state;
  memset(&state, 0, sizeof(state));
  state.fps = frames_per_second;
  state.band_count = band_count;
  state.stride = band_count + 1;

  int32_t error = nw_decoder_open(&decoder, path);
  if (error != NW_OK) goto cleanup;

  result->duration_ms = decoder.duration_ms;
  result->sample_rate = decoder.dec_ctx->sample_rate;

  if (result->duration_ms > 0) {
    const int64_t estimate = (result->duration_ms * frames_per_second) / 1000 + 8;
    if (estimate < INT32_MAX / state.stride && !ns_rows_reserve(&state, (int32_t)estimate)) {
      error = NW_ERR_ALLOC;
      goto cleanup;
    }
  }

  while (nw_decoder_next(&decoder)) {
    const AVFrame* frame = decoder.frame;
    if (state.sample_rate == 0) {
      // The container's idea of the rate is only a fallback: the decoder
      // reports the real one on the first frame.
      int sample_rate = frame->sample_rate > 0 ? frame->sample_rate : decoder.dec_ctx->sample_rate;
      if (sample_rate <= 0) sample_rate = 44100;
      result->sample_rate = sample_rate;
      if (!ns_state_prepare(&state, sample_rate)) {
        error = NW_ERR_ALLOC;
        goto cleanup;
      }
    }

    error = ns_append(&state, frame);
    if (error != NW_OK) goto cleanup;
    error = ns_drain(&state, INT64_MAX);
    if (error != NW_OK) goto cleanup;
  }

  if (state.sample_rate != 0) {
    // The last frames reach past the end of the audio, into silence.
    const int half = state.half;
    if (!ns_samples_reserve(&state, state.count + half)) {
      error = NW_ERR_ALLOC;
      goto cleanup;
    }
    memset(state.samples + state.count, 0, (size_t)half * sizeof(float));
    state.count += half;
    error = ns_drain(&state, state.total);
    if (error != NW_OK) goto cleanup;
  }

  if (state.row_count == 0) {
    error = NW_ERR_NO_OUTPUT;
    goto cleanup;
  }
  error = ns_finish(&state);

cleanup:
  nw_decoder_close(&decoder);
  ns_state_free(&state);

  result->data = state.rows;
  result->frame_count = state.row_count;
  result->band_count = band_count;
  result->frames_per_second = frames_per_second;
  result->error = error;
  return result;
}
