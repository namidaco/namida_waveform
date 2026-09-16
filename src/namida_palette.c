#include "namida_palette.h"

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

#define NP_VERSION 1

#define NP_DEFAULT_MAX_COLORS 16
#define NP_MAX_COLORS_LIMIT 256
#define NP_DEFAULT_MAX_HEIGHT 240
#define NP_MIN_HEIGHT 8
#define NP_IO_BUFFER_SIZE (32 * 1024)

/// Added to the decoder context's log level so a damaged file cannot spam the
/// log. Only the decoder supports this; the process wide level is left alone
/// because the host app uses it too.
#define NP_LOG_OFFSET 100

/// Colors are cut down to 5 bits per channel before counting, like Android's
/// Palette does, so the histogram is a fixed 32768-bin array rather than a map
/// and the quantizer sorts 16-bit bin indices instead of color objects.
#define NP_QUANT_SHIFT 3
#define NP_BIN_COUNT (1 << 15)
#define NP_BIN(r, g, b) ((((r) >> NP_QUANT_SHIFT) << 10) | (((g) >> NP_QUANT_SHIFT) << 5) | ((b) >> NP_QUANT_SHIFT))
#define NP_BIN_R(bin) ((((bin) >> 10) & 31) << NP_QUANT_SHIFT)
#define NP_BIN_G(bin) ((((bin) >> 5) & 31) << NP_QUANT_SHIFT)
#define NP_BIN_B(bin) (((bin) & 31) << NP_QUANT_SHIFT)

// -- in-memory input ---------------------------------------------------------

typedef struct {
  const uint8_t* data;
  int64_t size;
  int64_t pos;
} NPReader;

static int np_reader_read(void* opaque, uint8_t* buf, int buf_size) {
  NPReader* reader = (NPReader*)opaque;
  const int64_t left = reader->size - reader->pos;
  if (left <= 0) return AVERROR_EOF;
  if (buf_size > left) buf_size = (int)left;
  memcpy(buf, reader->data + reader->pos, (size_t)buf_size);
  reader->pos += buf_size;
  return buf_size;
}

static int64_t np_reader_seek(void* opaque, int64_t offset, int whence) {
  NPReader* reader = (NPReader*)opaque;
  int64_t base;
  switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET:
      base = 0;
      break;
    case SEEK_CUR:
      base = reader->pos;
      break;
    case SEEK_END:
      base = reader->size;
      break;
    case AVSEEK_SIZE:
      return reader->size;
    default:
      return AVERROR(EINVAL);
  }
  const int64_t pos = base + offset;
  if (pos < 0 || pos > reader->size) return AVERROR(EINVAL);
  reader->pos = pos;
  return pos;
}

// -- picture size without decoding -------------------------------------------

/// Reads the frame size off a JPEG's SOF marker, so the decoder can be asked
/// for a reduced resolution before it starts.
static int np_jpeg_dimensions(const uint8_t* p, int n, int* width, int* height) {
  if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return 0;
  int i = 2;
  while (i + 4 <= n) {
    if (p[i] != 0xFF) {
      i++;
      continue;
    }
    const uint8_t marker = p[i + 1];
    if (marker == 0xFF) {
      i++;
      continue;
    }
    // Standalone markers carry no length.
    if (marker == 0x01 || marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) {
      i += 2;
      continue;
    }
    // Scan data follows SOS; a SOF after it would be a second image.
    if (marker == 0xDA) return 0;
    const int length = (p[i + 2] << 8) | p[i + 3];
    if (length < 2) return 0;
    const int is_sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
    if (is_sof) {
      if (i + 9 > n) return 0;
      *height = (p[i + 5] << 8) | p[i + 6];
      *width = (p[i + 7] << 8) | p[i + 8];
      return *width > 0 && *height > 0;
    }
    i += 2 + length;
  }
  return 0;
}

// -- pixel sampling ----------------------------------------------------------

/// Byte offsets of the channels inside one packed pixel. 16-bit layouts point
/// at the high byte of each sample; `a < 0` means the format has no alpha.
typedef struct {
  int bpp;
  int r, g, b, a;
} NPPacked;

static int np_packed_layout(enum AVPixelFormat format, NPPacked* out) {
  switch (format) {
    case AV_PIX_FMT_RGB24: *out = (NPPacked){3, 0, 1, 2, -1}; return 1;
    case AV_PIX_FMT_BGR24: *out = (NPPacked){3, 2, 1, 0, -1}; return 1;
    case AV_PIX_FMT_RGBA: *out = (NPPacked){4, 0, 1, 2, 3}; return 1;
    case AV_PIX_FMT_BGRA: *out = (NPPacked){4, 2, 1, 0, 3}; return 1;
    case AV_PIX_FMT_ARGB: *out = (NPPacked){4, 1, 2, 3, 0}; return 1;
    case AV_PIX_FMT_ABGR: *out = (NPPacked){4, 3, 2, 1, 0}; return 1;
    case AV_PIX_FMT_RGB0: *out = (NPPacked){4, 0, 1, 2, -1}; return 1;
    case AV_PIX_FMT_BGR0: *out = (NPPacked){4, 2, 1, 0, -1}; return 1;
    case AV_PIX_FMT_0RGB: *out = (NPPacked){4, 1, 2, 3, -1}; return 1;
    case AV_PIX_FMT_0BGR: *out = (NPPacked){4, 3, 2, 1, -1}; return 1;
    case AV_PIX_FMT_GRAY8: *out = (NPPacked){1, 0, 0, 0, -1}; return 1;
    case AV_PIX_FMT_YA8: *out = (NPPacked){2, 0, 0, 0, 1}; return 1;
    case AV_PIX_FMT_GRAY16BE: *out = (NPPacked){2, 0, 0, 0, -1}; return 1;
    case AV_PIX_FMT_GRAY16LE: *out = (NPPacked){2, 1, 1, 1, -1}; return 1;
    case AV_PIX_FMT_YA16BE: *out = (NPPacked){4, 0, 0, 0, 2}; return 1;
    case AV_PIX_FMT_YA16LE: *out = (NPPacked){4, 1, 1, 1, 3}; return 1;
    case AV_PIX_FMT_RGB48BE: *out = (NPPacked){6, 0, 2, 4, -1}; return 1;
    case AV_PIX_FMT_RGB48LE: *out = (NPPacked){6, 1, 3, 5, -1}; return 1;
    case AV_PIX_FMT_BGR48BE: *out = (NPPacked){6, 4, 2, 0, -1}; return 1;
    case AV_PIX_FMT_BGR48LE: *out = (NPPacked){6, 5, 3, 1, -1}; return 1;
    case AV_PIX_FMT_RGBA64BE: *out = (NPPacked){8, 0, 2, 4, 6}; return 1;
    case AV_PIX_FMT_RGBA64LE: *out = (NPPacked){8, 1, 3, 5, 7}; return 1;
    case AV_PIX_FMT_BGRA64BE: *out = (NPPacked){8, 4, 2, 0, 6}; return 1;
    case AV_PIX_FMT_BGRA64LE: *out = (NPPacked){8, 5, 3, 1, 7}; return 1;
    default: return 0;
  }
}

/// Chroma layout of a planar 8-bit YUV format. `semi` means the chroma planes
/// are interleaved in `data[1]`, `swap_uv` that V comes first there.
typedef struct {
  int shift_x, shift_y;
  int full_range;
  int alpha;
  int semi;
  int swap_uv;
} NPYuv;

static int np_yuv_layout(enum AVPixelFormat format, NPYuv* out) {
  switch (format) {
    case AV_PIX_FMT_YUVJ420P: *out = (NPYuv){1, 1, 1, 0, 0, 0}; return 1;
    case AV_PIX_FMT_YUV420P: *out = (NPYuv){1, 1, 0, 0, 0, 0}; return 1;
    case AV_PIX_FMT_YUVJ422P: *out = (NPYuv){1, 0, 1, 0, 0, 0}; return 1;
    case AV_PIX_FMT_YUV422P: *out = (NPYuv){1, 0, 0, 0, 0, 0}; return 1;
    case AV_PIX_FMT_YUVJ444P: *out = (NPYuv){0, 0, 1, 0, 0, 0}; return 1;
    case AV_PIX_FMT_YUV444P: *out = (NPYuv){0, 0, 0, 0, 0, 0}; return 1;
    case AV_PIX_FMT_YUVJ440P: *out = (NPYuv){0, 1, 1, 0, 0, 0}; return 1;
    case AV_PIX_FMT_YUV440P: *out = (NPYuv){0, 1, 0, 0, 0, 0}; return 1;
    case AV_PIX_FMT_YUV411P: *out = (NPYuv){2, 0, 0, 0, 0, 0}; return 1;
    case AV_PIX_FMT_YUV410P: *out = (NPYuv){2, 2, 0, 0, 0, 0}; return 1;
    case AV_PIX_FMT_YUVA420P: *out = (NPYuv){1, 1, 0, 1, 0, 0}; return 1;
    case AV_PIX_FMT_YUVA422P: *out = (NPYuv){1, 0, 0, 1, 0, 0}; return 1;
    case AV_PIX_FMT_YUVA444P: *out = (NPYuv){0, 0, 0, 1, 0, 0}; return 1;
    case AV_PIX_FMT_NV12: *out = (NPYuv){1, 1, 0, 0, 1, 0}; return 1;
    case AV_PIX_FMT_NV21: *out = (NPYuv){1, 1, 0, 0, 1, 1}; return 1;
    default: return 0;
  }
}

static inline int np_clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/// BT.601 in 16.16 fixed point, the matrix JPEG and every other still image
/// format libavcodec hands us as YUV is defined against.
static inline void np_yuv_to_rgb(int y, int u, int v, int full_range, int* r, int* g, int* b) {
  u -= 128;
  v -= 128;
  if (full_range) {
    const int yy = (y << 16) + 32768;
    *r = np_clamp8((yy + 91881 * v) >> 16);
    *g = np_clamp8((yy - 22554 * u - 46802 * v) >> 16);
    *b = np_clamp8((yy + 116130 * u) >> 16);
  } else {
    const int yy = 76309 * (y - 16) + 32768;
    *r = np_clamp8((yy + 104597 * v) >> 16);
    *g = np_clamp8((yy - 25675 * u - 53279 * v) >> 16);
    *b = np_clamp8((yy + 132201 * u) >> 16);
  }
}

enum { NP_KIND_PACKED, NP_KIND_PAL8, NP_KIND_YUV, NP_KIND_GBR, NP_KIND_MONO };

/// Turns one row of accumulated cells into histogram entries and clears it.
static void np_flush_cells(uint32_t* acc, int sampled_width, uint32_t* hist) {
  for (int tx = 0; tx < sampled_width; tx++) {
    uint32_t* cell = acc + (size_t)tx * 4;
    const uint32_t n = cell[3];
    if (n != 0) {
      const int r = (int)((cell[0] + n / 2) / n);
      const int g = (int)((cell[1] + n / 2) / n);
      const int b = (int)((cell[2] + n / 2) / n);
      hist[NP_BIN(r, g, b)]++;
    }
    cell[0] = cell[1] = cell[2] = cell[3] = 0;
  }
}

/// Converts one source row into straight 8-bit RGBA, `0` alpha marking a
/// pixel to skip. Every pixel format this understands lands here, so the
/// sampler below has a single layout to average.
static void np_convert_row(const AVFrame* frame, int kind, const NPPacked* packed, const NPYuv* yuv, int sy,
                           uint8_t* out) {
  const int width = frame->width;
  const uint8_t* const* data = (const uint8_t* const*)frame->data;
  const int* linesize = frame->linesize;

  switch (kind) {
    case NP_KIND_PACKED: {
      const uint8_t* row = data[0] + (size_t)sy * linesize[0];
      for (int sx = 0; sx < width; sx++, out += 4) {
        const uint8_t* p = row + (size_t)sx * packed->bpp;
        out[0] = p[packed->r];
        out[1] = p[packed->g];
        out[2] = p[packed->b];
        out[3] = packed->a >= 0 ? p[packed->a] : 255;
      }
      break;
    }
    case NP_KIND_PAL8: {
      const uint32_t* palette = (const uint32_t*)data[1];
      const uint8_t* row = data[0] + (size_t)sy * linesize[0];
      for (int sx = 0; sx < width; sx++, out += 4) {
        const uint32_t c = palette[row[sx]];
        out[0] = (uint8_t)(c >> 16);
        out[1] = (uint8_t)(c >> 8);
        out[2] = (uint8_t)c;
        out[3] = (uint8_t)(c >> 24);
      }
      break;
    }
    case NP_KIND_YUV: {
      const int cy = sy >> yuv->shift_y;
      const uint8_t* y_row = data[0] + (size_t)sy * linesize[0];
      const uint8_t* u_row = data[1] + (size_t)cy * linesize[1];
      const uint8_t* v_row = yuv->semi ? u_row : data[2] + (size_t)cy * linesize[2];
      const uint8_t* a_row = yuv->alpha ? data[3] + (size_t)sy * linesize[3] : NULL;
      for (int sx = 0; sx < width; sx++, out += 4) {
        const int cx = sx >> yuv->shift_x;
        int u, v;
        if (yuv->semi) {
          u = u_row[2 * cx + yuv->swap_uv];
          v = v_row[2 * cx + 1 - yuv->swap_uv];
        } else {
          u = u_row[cx];
          v = v_row[cx];
        }
        int r, g, b;
        np_yuv_to_rgb(y_row[sx], u, v, yuv->full_range, &r, &g, &b);
        out[0] = (uint8_t)r;
        out[1] = (uint8_t)g;
        out[2] = (uint8_t)b;
        out[3] = a_row != NULL ? a_row[sx] : 255;
      }
      break;
    }
    case NP_KIND_GBR: {
      const uint8_t* g_row = data[0] + (size_t)sy * linesize[0];
      const uint8_t* b_row = data[1] + (size_t)sy * linesize[1];
      const uint8_t* r_row = data[2] + (size_t)sy * linesize[2];
      const uint8_t* a_row = frame->format == AV_PIX_FMT_GBRAP ? data[3] + (size_t)sy * linesize[3] : NULL;
      for (int sx = 0; sx < width; sx++, out += 4) {
        out[0] = r_row[sx];
        out[1] = g_row[sx];
        out[2] = b_row[sx];
        out[3] = a_row != NULL ? a_row[sx] : 255;
      }
      break;
    }
    case NP_KIND_MONO: {
      const int one_is_white = frame->format == AV_PIX_FMT_MONOBLACK;
      const uint8_t* row = data[0] + (size_t)sy * linesize[0];
      for (int sx = 0; sx < width; sx++, out += 4) {
        const int bit = (row[sx >> 3] >> (7 - (sx & 7))) & 1;
        const uint8_t value = (bit == one_is_white) ? 255 : 0;
        out[0] = out[1] = out[2] = value;
        out[3] = 255;
      }
      break;
    }
  }
}

/// Counts the picture's pixels into `hist`, box-averaged onto a grid at most
/// `max_height` rows tall, the width following the aspect ratio: every source
/// pixel contributes to the cell it falls in, like a filtered downscale would,
/// without producing the scaled bitmap. Fully transparent pixels are skipped,
/// as Palette does, and a cell holding nothing else is dropped. Returns 0 for
/// a pixel format this cannot read.
static int np_sample_frame(const AVFrame* frame, int max_height, uint32_t* hist) {
  const int width = frame->width;
  const int height = frame->height;
  if (width <= 0 || height <= 0) return 0;

  int sampled_width = width;
  int sampled_height = height;
  if (height > max_height) {
    sampled_height = max_height;
    sampled_width = (int)(((int64_t)width * max_height + height / 2) / height);
    if (sampled_width < 1) sampled_width = 1;
  }

  const enum AVPixelFormat format = (enum AVPixelFormat)frame->format;
  NPPacked packed = {0, 0, 0, 0, -1};
  NPYuv yuv = {0, 0, 0, 0, 0, 0};
  int kind;
  if (np_packed_layout(format, &packed)) {
    kind = NP_KIND_PACKED;
  } else if (format == AV_PIX_FMT_PAL8) {
    kind = NP_KIND_PAL8;
  } else if (np_yuv_layout(format, &yuv)) {
    kind = NP_KIND_YUV;
    if (frame->color_range == AVCOL_RANGE_JPEG) yuv.full_range = 1;
  } else if (format == AV_PIX_FMT_GBRP || format == AV_PIX_FMT_GBRAP) {
    kind = NP_KIND_GBR;
  } else if (format == AV_PIX_FMT_MONOBLACK || format == AV_PIX_FMT_MONOWHITE) {
    kind = NP_KIND_MONO;
  } else {
    return 0;
  }

  // Sums of r, g, b and the pixel count per grid column of the current cell row.
  uint32_t* acc = (uint32_t*)calloc((size_t)sampled_width * 4, sizeof(uint32_t));
  uint8_t* rgba = (uint8_t*)malloc((size_t)width * 4);
  int* col_of = (int*)malloc((size_t)width * sizeof(int));
  if (acc == NULL || rgba == NULL || col_of == NULL) {
    free(acc);
    free(rgba);
    free(col_of);
    return 0;
  }
  for (int sx = 0; sx < width; sx++) col_of[sx] = (int)(((int64_t)sx * sampled_width) / width);

  int cell_row = 0;
  for (int sy = 0; sy < height; sy++) {
    const int ty = (int)(((int64_t)sy * sampled_height) / height);
    if (ty != cell_row) {
      np_flush_cells(acc, sampled_width, hist);
      cell_row = ty;
    }
    np_convert_row(frame, kind, &packed, &yuv, sy, rgba);
    const uint8_t* p = rgba;
    for (int sx = 0; sx < width; sx++, p += 4) {
      if (p[3] == 0) continue;
      uint32_t* cell = acc + (size_t)col_of[sx] * 4;
      cell[0] += p[0];
      cell[1] += p[1];
      cell[2] += p[2];
      cell[3]++;
    }
  }
  np_flush_cells(acc, sampled_width, hist);

  free(col_of);
  free(rgba);
  free(acc);
  return 1;
}

// -- median cut --------------------------------------------------------------
//
// A port of Android's ColorCutQuantizer (the one palette_generator ports too),
// working on histogram bin indices: sorting a box by a channel is sorting
// 16-bit keys, and a box's colors are a contiguous range of the index array.

typedef struct {
  int lower, upper;
  int min_r, max_r, min_g, max_g, min_b, max_b;
  uint32_t population;
} NPBox;

static void np_box_fit(NPBox* box, const uint16_t* bins, const uint32_t* hist) {
  int min_r = 256, min_g = 256, min_b = 256;
  int max_r = -1, max_g = -1, max_b = -1;
  uint32_t population = 0;
  for (int i = box->lower; i <= box->upper; i++) {
    const int bin = bins[i];
    const int r = NP_BIN_R(bin), g = NP_BIN_G(bin), b = NP_BIN_B(bin);
    population += hist[bin];
    if (r > max_r) max_r = r;
    if (r < min_r) min_r = r;
    if (g > max_g) max_g = g;
    if (g < min_g) min_g = g;
    if (b > max_b) max_b = b;
    if (b < min_b) min_b = b;
  }
  box->min_r = min_r;
  box->max_r = max_r;
  box->min_g = min_g;
  box->max_g = max_g;
  box->min_b = min_b;
  box->max_b = max_b;
  box->population = population;
}

static inline int64_t np_box_volume(const NPBox* box) {
  return (int64_t)(box->max_r - box->min_r + 1) * (box->max_g - box->min_g + 1) * (box->max_b - box->min_b + 1);
}

static inline int np_box_color_count(const NPBox* box) { return box->upper - box->lower + 1; }

// Bin indices already order by (r, g, b); the other two orders are rebuilt
// from the same 5-bit fields.
static int np_compare_r(const void* a, const void* b) {
  return (int)*(const uint16_t*)a - (int)*(const uint16_t*)b;
}

static inline int np_key_g(uint16_t bin) { return ((bin >> 5) & 31) << 10 | ((bin >> 10) & 31) << 5 | (bin & 31); }
static inline int np_key_b(uint16_t bin) { return (bin & 31) << 10 | ((bin >> 5) & 31) << 5 | ((bin >> 10) & 31); }

static int np_compare_g(const void* a, const void* b) {
  return np_key_g(*(const uint16_t*)a) - np_key_g(*(const uint16_t*)b);
}

static int np_compare_b(const void* a, const void* b) {
  return np_key_b(*(const uint16_t*)a) - np_key_b(*(const uint16_t*)b);
}

/// Sorts the box along its longest dimension and returns the index at which
/// the population is split in half.
static int np_box_split_point(const NPBox* box, uint16_t* bins, const uint32_t* hist) {
  const int len_r = box->max_r - box->min_r;
  const int len_g = box->max_g - box->min_g;
  const int len_b = box->max_b - box->min_b;
  int (*compare)(const void*, const void*);
  if (len_r >= len_g && len_r >= len_b) {
    compare = np_compare_r;
  } else if (len_g >= len_r && len_g >= len_b) {
    compare = np_compare_g;
  } else {
    compare = np_compare_b;
  }
  qsort(bins + box->lower, (size_t)np_box_color_count(box), sizeof(uint16_t), compare);

  const uint32_t median = (box->population + 1) / 2;
  uint32_t count = 0;
  for (int i = box->lower; i <= box->upper; i++) {
    count += hist[bins[i]];
    if (count >= median) {
      // Never split on the upper index, that would leave the box unchanged.
      return i < box->upper - 1 ? i : box->upper - 1;
    }
  }
  return box->lower;
}

static void np_box_split(NPBox* box, NPBox* out_new, uint16_t* bins, const uint32_t* hist) {
  const int split = np_box_split_point(box, bins, hist);
  out_new->lower = split + 1;
  out_new->upper = box->upper;
  np_box_fit(out_new, bins, hist);
  box->upper = split;
  np_box_fit(box, bins, hist);
}

typedef struct {
  uint32_t color;
  uint32_t population;
} NPSwatch;

static int np_compare_swatch(const void* a, const void* b) {
  const uint32_t pa = ((const NPSwatch*)a)->population;
  const uint32_t pb = ((const NPSwatch*)b)->population;
  return pa < pb ? 1 : (pa > pb ? -1 : 0);
}

static inline uint32_t np_argb(int r, int g, int b) {
  return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/// Reduces `hist` to at most `max_colors` swatches, most populous first.
/// `bins` is scratch space for `NP_BIN_COUNT` entries.
static int32_t np_quantize(const uint32_t* hist, uint16_t* bins, int32_t max_colors, NPSwatch* out) {
  int distinct = 0;
  for (int bin = 0; bin < NP_BIN_COUNT; bin++) {
    if (hist[bin] != 0) bins[distinct++] = (uint16_t)bin;
  }
  if (distinct == 0) return 0;

  int32_t count = 0;
  if (distinct <= max_colors) {
    for (int i = 0; i < distinct; i++) {
      const int bin = bins[i];
      out[count].color = np_argb(NP_BIN_R(bin), NP_BIN_G(bin), NP_BIN_B(bin));
      out[count].population = hist[bin];
      count++;
    }
  } else {
    NPBox* boxes = (NPBox*)malloc((size_t)max_colors * sizeof(NPBox));
    if (boxes == NULL) return 0;
    boxes[0].lower = 0;
    boxes[0].upper = distinct - 1;
    np_box_fit(&boxes[0], bins, hist);
    int box_count = 1;

    // Always split the box with the largest volume; stop as soon as that one
    // is down to a single color, which is what the priority queue port does.
    while (box_count < max_colors) {
      int largest = 0;
      for (int i = 1; i < box_count; i++) {
        if (np_box_volume(&boxes[i]) > np_box_volume(&boxes[largest])) largest = i;
      }
      if (np_box_color_count(&boxes[largest]) <= 1) break;
      np_box_split(&boxes[largest], &boxes[box_count], bins, hist);
      box_count++;
    }

    for (int i = 0; i < box_count; i++) {
      const NPBox* box = &boxes[i];
      uint64_t sum_r = 0, sum_g = 0, sum_b = 0;
      for (int j = box->lower; j <= box->upper; j++) {
        const int bin = bins[j];
        const uint64_t population = hist[bin];
        sum_r += population * (uint64_t)NP_BIN_R(bin);
        sum_g += population * (uint64_t)NP_BIN_G(bin);
        sum_b += population * (uint64_t)NP_BIN_B(bin);
      }
      const uint64_t total = box->population;
      if (total == 0) continue;
      const int r = (int)((sum_r + total / 2) / total);
      const int g = (int)((sum_g + total / 2) / total);
      const int b = (int)((sum_b + total / 2) / total);
      out[count].color = np_argb(r, g, b);
      out[count].population = (uint32_t)total;
      count++;
    }
    free(boxes);
  }

  qsort(out, (size_t)count, sizeof(NPSwatch), np_compare_swatch);
  return count;
}

// -- extraction --------------------------------------------------------------

static NPResult* np_fail(NPResult* result, int32_t error) {
  if (result != NULL) result->error = error;
  return result;
}

NP_EXPORT int32_t np_version(void) { return NP_VERSION; }

NP_EXPORT void np_result_free(NPResult* result) {
  if (result == NULL) return;
  free(result->colors);
  free(result->populations);
  free(result);
}

NP_EXPORT NPResult* np_extract(const char* path, const uint8_t* data, int64_t data_size, int32_t max_colors,
                               int32_t max_height) {
  NPResult* result = (NPResult*)calloc(1, sizeof(NPResult));
  if (result == NULL) return NULL;

  if (data == NULL || data_size <= 0) {
    data = NULL;
    if (path == NULL) return np_fail(result, NP_ERR_OPEN_INPUT);
  }
  if (max_colors <= 0) max_colors = NP_DEFAULT_MAX_COLORS;
  if (max_colors > NP_MAX_COLORS_LIMIT) max_colors = NP_MAX_COLORS_LIMIT;
  if (max_height <= 0) max_height = NP_DEFAULT_MAX_HEIGHT;
  if (max_height < NP_MIN_HEIGHT) max_height = NP_MIN_HEIGHT;

  AVIOContext* io = NULL;
  AVFormatContext* fmt_ctx = NULL;
  AVCodecContext* dec_ctx = NULL;
  AVPacket* packet = NULL;
  AVFrame* frame = NULL;
  uint32_t* hist = NULL;
  uint16_t* bins = NULL;
  NPSwatch* swatches = NULL;
  NPReader reader = {data, data_size, 0};
  int32_t error = NP_OK;

  if (data != NULL) {
    uint8_t* io_buffer = (uint8_t*)av_malloc(NP_IO_BUFFER_SIZE);
    if (io_buffer == NULL) return np_fail(result, NP_ERR_ALLOC);
    io = avio_alloc_context(io_buffer, NP_IO_BUFFER_SIZE, 0, &reader, np_reader_read, NULL, np_reader_seek);
    if (io == NULL) {
      av_free(io_buffer);
      return np_fail(result, NP_ERR_ALLOC);
    }
  } else if (avio_open(&io, path, AVIO_FLAG_READ) < 0) {
    return np_fail(result, NP_ERR_OPEN_INPUT);
  }

  fmt_ctx = avformat_alloc_context();
  if (fmt_ctx == NULL) {
    error = NP_ERR_ALLOC;
    goto cleanup;
  }
  // Handing over the I/O context instead of the path makes probing look at the
  // bytes only: image2 would otherwise claim the file by its extension and
  // pick the wrong decoder for a cache file whose name lies about its format.
  fmt_ctx->pb = io;
  if (avformat_open_input(&fmt_ctx, NULL, NULL, NULL) < 0) {
    // On failure avformat_open_input already freed the context; custom I/O
    // is left to its owner.
    error = NP_ERR_OPEN_INPUT;
    goto cleanup;
  }

  int stream_index = -1;
  for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      stream_index = (int)i;
    } else {
      fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
    }
    if (stream_index >= 0) break;
  }
  if (stream_index < 0) {
    error = NP_ERR_NO_VIDEO_STREAM;
    goto cleanup;
  }
  AVStream* stream = fmt_ctx->streams[stream_index];

  const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  if (decoder == NULL) {
    error = NP_ERR_NO_DECODER;
    goto cleanup;
  }

  packet = av_packet_alloc();
  frame = av_frame_alloc();
  if (packet == NULL || frame == NULL) {
    error = NP_ERR_ALLOC;
    goto cleanup;
  }

  // The first picture is read before the decoder opens: its size can then be
  // taken off the bitstream and used to pick a reduced decoding resolution,
  // which avformat_find_stream_info could only tell us by decoding it in full.
  for (;;) {
    if (av_read_frame(fmt_ctx, packet) < 0) {
      error = NP_ERR_DECODE;
      goto cleanup;
    }
    if (packet->stream_index == stream_index) break;
    av_packet_unref(packet);
  }

  dec_ctx = avcodec_alloc_context3(decoder);
  if (dec_ctx == NULL) {
    error = NP_ERR_ALLOC;
    goto cleanup;
  }
  if (avcodec_parameters_to_context(dec_ctx, stream->codecpar) < 0) {
    error = NP_ERR_DECODER_OPEN;
    goto cleanup;
  }
  dec_ctx->log_level_offset = NP_LOG_OFFSET;
  // One picture: frame threads would only add a pool to spin up and tear down.
  dec_ctx->thread_count = 1;

  // JPEG decodes at 1/2, 1/4 or 1/8 straight out of the DCT, which is nearly
  // free; ask for the largest reduction that still covers the sampling grid.
  if (decoder->max_lowres > 0 && stream->codecpar->codec_id == AV_CODEC_ID_MJPEG) {
    int width, height;
    if (np_jpeg_dimensions(packet->data, packet->size, &width, &height)) {
      int lowres = 0;
      while (lowres < decoder->max_lowres && (height >> (lowres + 1)) >= max_height) lowres++;
      dec_ctx->lowres = lowres;
    }
  }

  if (avcodec_open2(dec_ctx, decoder, NULL) < 0) {
    error = NP_ERR_DECODER_OPEN;
    goto cleanup;
  }

  const int sent = avcodec_send_packet(dec_ctx, packet);
  av_packet_unref(packet);
  if (sent < 0) {
    error = NP_ERR_DECODE;
    goto cleanup;
  }
  // Flush, so a decoder that holds pictures back hands this one over.
  avcodec_send_packet(dec_ctx, NULL);
  if (avcodec_receive_frame(dec_ctx, frame) < 0) {
    error = NP_ERR_DECODE;
    goto cleanup;
  }
  result->width = frame->width;
  result->height = frame->height;

  hist = (uint32_t*)calloc(NP_BIN_COUNT, sizeof(uint32_t));
  bins = (uint16_t*)malloc(NP_BIN_COUNT * sizeof(uint16_t));
  swatches = (NPSwatch*)malloc((size_t)max_colors * sizeof(NPSwatch));
  if (hist == NULL || bins == NULL || swatches == NULL) {
    error = NP_ERR_ALLOC;
    goto cleanup;
  }

  if (!np_sample_frame(frame, max_height, hist)) {
    error = NP_ERR_UNSUPPORTED_FORMAT;
    goto cleanup;
  }

  const int32_t count = np_quantize(hist, bins, max_colors, swatches);
  if (count <= 0) {
    error = NP_ERR_NO_OUTPUT;
    goto cleanup;
  }

  result->colors = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
  result->populations = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
  if (result->colors == NULL || result->populations == NULL) {
    error = NP_ERR_ALLOC;
    goto cleanup;
  }
  for (int32_t i = 0; i < count; i++) {
    result->colors[i] = swatches[i].color;
    result->populations[i] = swatches[i].population;
  }
  result->count = count;

cleanup:
  free(swatches);
  free(bins);
  free(hist);
  if (frame != NULL) av_frame_free(&frame);
  if (packet != NULL) av_packet_free(&packet);
  if (dec_ctx != NULL) avcodec_free_context(&dec_ctx);
  if (fmt_ctx != NULL) avformat_close_input(&fmt_ctx);
  if (io != NULL) {
    if (data != NULL) {
      // avformat may have swapped the buffer for a larger one meanwhile.
      av_freep(&io->buffer);
      avio_context_free(&io);
    } else {
      avio_closep(&io);
    }
  }
  if (error != NP_OK) {
    free(result->colors);
    free(result->populations);
    result->colors = NULL;
    result->populations = NULL;
    result->count = 0;
  }
  result->error = error;
  return result;
}

// -- worker thread -----------------------------------------------------------

typedef struct {
  char* path;
  uint8_t* data;
  int64_t data_size;
  int32_t max_colors;
  int32_t max_height;
  int64_t request_id;
  np_callback callback;
} NPRequest;

static void np_request_free(NPRequest* request) {
  free(request->path);
  free(request->data);
  free(request);
}

static void np_request_run(NPRequest* request) {
  NPResult* result = np_extract(request->path, request->data, request->data_size, request->max_colors,
                                request->max_height);
  request->callback(request->request_id, result);
  np_request_free(request);
}

#if defined(_WIN32)
static unsigned __stdcall np_thread_main(void* arg) {
  np_request_run((NPRequest*)arg);
  return 0;
}

static int np_spawn(NPRequest* request) {
  const uintptr_t handle = _beginthreadex(NULL, 0, np_thread_main, request, 0, NULL);
  if (handle == 0) return 0;
  CloseHandle((HANDLE)handle);
  return 1;
}
#else
static void* np_thread_main(void* arg) {
  np_request_run((NPRequest*)arg);
  return NULL;
}

static int np_spawn(NPRequest* request) {
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return 0;
  pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  const int rc = pthread_create(&thread, &attributes, np_thread_main, request);
  pthread_attr_destroy(&attributes);
  return rc == 0;
}
#endif

NP_EXPORT int32_t np_extract_async(const char* path, const uint8_t* data, int64_t data_size, int32_t max_colors,
                                   int32_t max_height, int64_t request_id, np_callback callback) {
  if (callback == NULL) return NP_ERR_THREAD;
  if (data == NULL || data_size <= 0) {
    data = NULL;
    if (path == NULL) return NP_ERR_OPEN_INPUT;
  }

  NPRequest* request = (NPRequest*)calloc(1, sizeof(NPRequest));
  if (request == NULL) return NP_ERR_ALLOC;
  if (data != NULL) {
    request->data = (uint8_t*)malloc((size_t)data_size);
    if (request->data == NULL) {
      np_request_free(request);
      return NP_ERR_ALLOC;
    }
    memcpy(request->data, data, (size_t)data_size);
    request->data_size = data_size;
  } else {
    const size_t length = strlen(path) + 1;
    request->path = (char*)malloc(length);
    if (request->path == NULL) {
      np_request_free(request);
      return NP_ERR_ALLOC;
    }
    memcpy(request->path, path, length);
  }
  request->max_colors = max_colors;
  request->max_height = max_height;
  request->request_id = request_id;
  request->callback = callback;

  if (!np_spawn(request)) {
    np_request_free(request);
    return NP_ERR_THREAD;
  }
  return NP_OK;
}
