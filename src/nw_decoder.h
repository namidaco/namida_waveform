#ifndef NW_DECODER_H
#define NW_DECODER_H

#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The best audio stream of a file, decoded one frame at a time. Shared by the
/// waveform and spectrum extractors, internal to the library.
typedef struct {
  AVFormatContext* fmt_ctx;
  AVCodecContext* dec_ctx;
  /// The frame `nw_decoder_next` last produced, valid until the next call.
  AVFrame* frame;
  AVPacket* packet;
  int stream_index;
  /// Source duration in milliseconds, `0` when the container reports none.
  int64_t duration_ms;
  int draining;
  /// Set while `packet` still holds data the decoder refused with EAGAIN; it is
  /// resent once the frames it was waiting on have been read.
  int pending;
  int receiving;
  int received;
} NWDecoder;

/// Returns `NW_OK` or one of the `NW_ERR_*` codes. `nw_decoder_close` has to be
/// called either way.
int32_t nw_decoder_open(NWDecoder* decoder, const char* path);

/// Returns 1 with `decoder->frame` holding the next frame, 0 once the stream
/// has ended.
int nw_decoder_next(NWDecoder* decoder);

void nw_decoder_close(NWDecoder* decoder);

/// Averages every channel of `frame` into `frame->nb_samples` mono samples at
/// `dst`, in the `-1..1` domain. Returns `NW_OK` or `NW_ERR_UNSUPPORTED_FORMAT`.
int32_t nw_decoder_downmix(const AVFrame* frame, float* dst);

#ifdef __cplusplus
}
#endif

#endif  // NW_DECODER_H
