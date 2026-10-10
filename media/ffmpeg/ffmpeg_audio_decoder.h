// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Original implementation (BSD-3-Clause, LICENSE §3). The structure follows
// Chromium's media::FFmpegAudioDecoder: an asynchronous AudioDecoder over
// avcodec send/receive. It fills the ROLE ffplay's audio_decode_frame() plays
// in ijkplayer (documented in docs/05), but shares no line or algorithm with
// it -- an earlier header comment said "ported from"; that overstated the
// relationship and was corrected in PROGRESS round 14 (decision A).

#ifndef AVBASE_MEDIA_FFMPEG_AUDIO_DECODER_H_
#define AVBASE_MEDIA_FFMPEG_AUDIO_DECODER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "media/base/audio_buffer.h"
#include "media/base/audio_decoder.h"
#include "media/base/decoder_config.h"
#include "media/media_export.h"

namespace avbase::media {

// libavcodec-backed audio decoder producing AudioBuffer.
//
// Unlike ijkplayer, which resamples every decoded frame inside
// audio_decode_frame() via swr_convert on the audio callback thread, this
// decoder emits buffers in the codec's native sample format and leaves
// conversion to the consumer (AudioBuffer::ReadFrames). That keeps the decoder
// free of any audio-device dependency, and avoids a resampler context being
// torn down and rebuilt whenever the output device changes.
//
// All FFmpeg handles live in an opaque Context so this header never includes
// libav*.h (invariant C4).
class AVBASE_MEDIA_EXPORT FFmpegAudioDecoder final : public AudioDecoder {
 public:
  FFmpegAudioDecoder();
  ~FFmpegAudioDecoder() override;
  FFmpegAudioDecoder(const FFmpegAudioDecoder&) = delete;
  FFmpegAudioDecoder& operator=(const FFmpegAudioDecoder&) = delete;

  // AudioDecoder
  std::string GetDisplayName() const override { return "FFmpegAudioDecoder"; }
  bool IsPlatformDecoder() const override { return false; }
  void Initialize(const AudioDecoderConfig& config, bool has_pending_clear,
                  int32_t current_serial, InitCB init_cb,
                  const OutputCB& output_cb,
                  const WaitingCB& waiting_cb) override;
  void Decode(base::scoped_refptr<DecoderBuffer> buffer,
              DecodeCB decode_cb) override;
  void Reset(base::OnceClosure closure) override;

  bool initialized() const { return initialized_; }

 private:
  struct Context;

  // Opens libavcodec for |config| and resolves the decoder's real output
  // format. Split out of Initialize() so that Initialize() stays a
  // validate/open/notify sequence rather than a wall of FFmpeg calls.
  DecoderStatus OpenCodec(const AudioDecoderConfig& config);

  // Emits every frame libavcodec currently holds. Returns false on a hard
  // error, true when the decoder is simply waiting for more input (EAGAIN) or
  // has drained (EOF).
  bool DecodeAvailableFrames(bool* drained);

  std::unique_ptr<Context> ctx_;
  OutputCB output_cb_;
  WaitingCB waiting_cb_;
  bool initialized_{false};
  int32_t serial_{0};
  SampleFormat sample_format_{SampleFormat::kUnknown};
  ChannelLayout channel_layout_{ChannelLayout::kNone};
  int channels_{0};
  int sample_rate_{0};
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FFMPEG_AUDIO_DECODER_H_
