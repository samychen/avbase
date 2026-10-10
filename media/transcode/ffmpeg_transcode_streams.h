// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_TRANSCODE_TRANSCODE_STREAMS_H_
#define AVBASE_MEDIA_TRANSCODE_TRANSCODE_STREAMS_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "media/base/audio_buffer.h"
#include "media/base/media_error.h"
#include "media/base/video_frame.h"
#include "media/media_export.h"
#include "media/transcode/ffmpeg_audio_encoder.h"
#include "media/transcode/ffmpeg_encode_muxer.h"
#include "media/transcode/ffmpeg_transcode_job.h"
#include "media/transcode/ffmpeg_video_encoder.h"

// Forward-declared, not included: this header is compiled into the same
// target as the transcode job but stays free of libav types so nothing else
// in media/ inherits them. Frame rates are therefore carried as separate
// num/den integers rather than as an AVRational value member.
struct AVCodecContext;
struct AVFormatContext;
struct AVAudioFifo;
struct AVFrame;
struct SwsContext;

namespace avbase::media {

// Forward-declared so this header need not include the Swr wrapper; the full
// definition arrives via media/ffmpeg/audio_convert.h in the .cc files that
// own AudioState.
namespace ffmpeg {
class AudioConverter;
}  // namespace ffmpeg

// unique_ptr deleter for AVCodecContext. Defined in the .cc precisely so
// that this header does not need avcodec.h.
struct AVBASE_MEDIA_EXPORT CodecCtxDeleter {
  void operator()(AVCodecContext* p) const;
};
using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxDeleter>;

// unique_ptr deleter for AVFrame, for the same reason.
struct AVBASE_MEDIA_EXPORT AvFrameDeleter {
  void operator()(AVFrame* p) const;
};
using AvFramePtr = std::unique_ptr<AVFrame, AvFrameDeleter>;

// Per-stream state for the transcode loop. Everything the loop needs to know
// about a stream, and nothing about how it was opened — that is
// PrepareAudioStream()/PrepareVideoStream()'s job.
struct AudioState {
  bool copy = false;
  CodecCtxPtr decoder;
  FFmpegAudioEncoder encoder;
  // Input geometry (from the decoder).
  int in_sample_rate = 0;
  int in_channels = 0;
  // Output geometry.
  int out_sample_rate = 0;
  int out_channels = 0;
  int muxer_stream_index = -1;
  // Timestamp units, as num/den pairs so this header stays free of libav.
  // |in_tb| is what the demuxer hands us; |out_tb| is what the muxer was
  // told to expect. They coincide for mp4 audio (1/sample_rate) and differ
  // for everything else — mkv reports 1/1000 and adts 1/28224000 for the
  // very same AAC payload — so the copy path has to convert between them
  // rather than forward the integers. See RescaleTimestamps().
  int in_tb_num = 1;
  int in_tb_den = 48000;
  int out_tb_num = 1;
  int out_tb_den = 48000;
  // Resampler, created on first use and only when the decoder and the
  // encoder disagree on sample rate or channel count. It is a thin wrapper
  // over libswresample (media/ffmpeg/audio_convert.h); the FIFO framing
  // below is what turns its variable-length output into encoder-sized whole
  // frames, because libavcodec rejects an arbitrary sample count with
  // EINVAL. |framed| is the scratch the FIFO is drained into.
  std::unique_ptr<ffmpeg::AudioConverter> resampler;
  AVAudioFifo* fifo = nullptr;
  AvFramePtr framed;
  // Not defined inline: releasing SwrContext needs libav, and this header
  // deliberately does not include it.
  ~AudioState();
};

struct VideoState {
  bool copy = false;
  CodecCtxPtr decoder;
  FFmpegVideoEncoder encoder;
  // Input geometry.
  int in_width = 0;
  int in_height = 0;
  int in_fps_num = 0;
  int in_fps_den = 1;
  // Output geometry.
  int out_width = 0;
  int out_height = 0;
  int out_fps_num = 0;
  int out_fps_den = 1;
  // Timestamp units; see the note on AudioState above.
  int in_tb_num = 1;
  int in_tb_den = 90000;
  int out_tb_num = 1;
  int out_tb_den = 90000;
  int muxer_stream_index = -1;
  SwsContext* sws = nullptr;
  bool sws_ready = false;
};

// Conversions from a decoded libav frame into avbase's own buffer types.
// |pts_time_base| is the time_base the frame's pts is expressed in -- the
// decoder context's pkt_timebase (= the input stream's time_base). It must
// NOT be assumed to be 1/90000: containers use 1/1000, 1/48000, 1/25 ...
// and a hardcoded denominator silently rewrites every timestamp.
AVBASE_MEDIA_EXPORT
base::scoped_refptr<AudioBuffer>
AvFrameToAudioBuffer(AVFrame* frame, int sample_rate, int channels);
AVBASE_MEDIA_EXPORT
base::scoped_refptr<VideoFrame> AvFrameToVideoFrame(AVFrame* frame, int width,
                                                    int height,
                                                    AVRational pts_time_base);

// Timeline helpers shared by the transcode loop and the concat job's seam:
// every stream's first written timestamp becomes its origin so that output
// timelines start at zero (see the note in ffmpeg_concat_job.cc for what
// happens when they do not).
AVBASE_MEDIA_EXPORT
int64_t TimelineOrigin(bool* seen, int64_t* origin, int64_t ts);
AVBASE_MEDIA_EXPORT
int64_t ShiftTs(int64_t ts, int64_t origin);

// Converts a packet's timestamps from the units it arrived in into the units
// the muxer was told to expect. Stream copy is where the two diverge: the
// demuxer's timebase is a property of the input container while the muxer's
// source timebase is derived from the codec, and only mp4 happens to make
// them equal. Timestamps that are unset are passed through untouched —
// av_rescale_q() on AV_NOPTS_VALUE yields garbage, not "still unset".
AVBASE_MEDIA_EXPORT
void RescaleTimestamps(EncodedPacket* ep, int in_num, int in_den, int out_num,
                       int out_den);

// Converts a decoded frame into zero or more encoder-sized buffers.
// Returns exactly one buffer — the frame as decoded — when the decoder and
// encoder already agree on sample rate and channel count, so the common path
// is unchanged. When they disagree, the frame is resampled and buffered, and
// however many WHOLE codec frames that yields are returned (often zero:
// resampling 44100→24000 turns 1024 samples into 557, not a whole frame).
//
// Without this, asking for a sample rate the input does not have simply
// relabels the same audio: 16 frames of 44100 Hz asked for 24000 Hz come out
// 938ms long instead of 488ms, because the encoder counts the samples it was
// handed at the rate it was told rather than the rate they were taken at.
AVBASE_MEDIA_EXPORT
std::vector<base::scoped_refptr<AudioBuffer>>
FramesForEncoder(AudioState* st, AVFrame* frame, int frame_size);

// Drains what the resampler is still holding: the remaining whole frames,
// then the tail as a final short frame. Empty when nothing was resampled.
AVBASE_MEDIA_EXPORT
std::vector<base::scoped_refptr<AudioBuffer>> FlushResampler(AudioState* st,
                                                             int frame_size);

// Stream preparation: opens the decoder (unless the stream is copied),
// initializes the encoder — resolving its concrete name through the E5
// factory list — and registers the output stream on |muxer|. Split out of
// the transcode loop (invariant C1) because preparation and pumping are
// different concerns even though they share the states above.
AVBASE_MEDIA_EXPORT
Status PrepareAudioStream(AVFormatContext* in_ctx, int audio_stream_idx,
                          const TranscodeParams& params,
                          FFmpegEncodeMuxer* muxer, AudioState* out);
AVBASE_MEDIA_EXPORT
Status PrepareVideoStream(AVFormatContext* in_ctx, int video_stream_idx,
                          const TranscodeParams& params,
                          FFmpegEncodeMuxer* muxer, VideoState* out);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_TRANSCODE_TRANSCODE_STREAMS_H_
