// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Sample-rate / channel-layout conversion for the FFmpeg audio path: a thin,
// stateful wrapper over libswresample (SwrContext). This is the audio mirror
// of media/ffmpeg/video_convert.h (VideoConverter over libswscale): both live
// in the vendor-quarantine layer (C4) and expose FFmpeg types only at their
// own boundary, never to avbase's media/base or media/filters layers.
//
// Why a dedicated class when the transcode path used to inline the Swr setup:
// the architecture doc (docs/02 §4.1) lists ffmpeg_audio_converter ->
// media::AudioConverter as a planned component that was never extracted, and
// the inline version bypassed the ff::MakeSwrContext factory (Round46's
// checked-allocator convention). Pulling it out makes the resampler
// independently testable (see audio_convert_unittest.cc) and keeps the
// vendor code grouped with its video sibling.
//
// Why Push() is stateful and frame-buffered while VideoConverter::Convert() is
// one-in-one-out: resampling is accumulative. N input samples do not yield a
// fixed M output samples -- the resampler carries delay, so one input frame
// may produce zero, one, or several output frames. The caller is responsible
// for any further framing (e.g. chopping the output into encoder-sized whole
// frames); that concern is deliberately NOT here, because frame_size belongs
// to the encoder, not to the resampler.

#ifndef AVBASE_MEDIA_FFMPEG_AUDIO_CONVERT_H_
#define AVBASE_MEDIA_FFMPEG_AUDIO_CONVERT_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"
#include "media/media_export.h"

namespace avbase::media::ffmpeg {

// RAII sample-rate / channel-layout converter. Configure() pins the in->out
// geometry; Push() converts one input frame into zero or more resampled
// output frames; Flush() drains the resampler's tail. Every produced frame is
// an owned AVFrame (FramePtr) in the configured output format.
class AVBASE_MEDIA_EXPORT AudioConverter {
 public:
  AudioConverter();
  ~AudioConverter();
  AudioConverter(const AudioConverter&) = delete;
  AudioConverter& operator=(const AudioConverter&) = delete;

  // Pins the in->out geometry. A no-op when the geometry is unchanged;
  // rebuilds the SwrContext otherwise. Returns false only if the context
  // cannot be created (a malformed format/layout). |in_fmt| is the decoder's
  // output sample format; |out_fmt| is what the encoder expects (FLTP here).
  bool Configure(int in_rate, int in_channels, AVSampleFormat in_fmt,
                 int out_rate, int out_channels, AVSampleFormat out_fmt);

  // True between a successful Configure() and Reset().
  bool Configured() const { return static_cast<bool>(swr_); }
  void Reset();

  // Converts |src| and returns the resampled audio as zero or more owned
  // frames. An empty result is NORMAL (the resampler may be holding samples
  // back for the next call or for Flush()), not an error.
  std::vector<FramePtr> Push(const AVFrame& src);

  // Feeds the resampler a null input to release what it is still holding, and
  // returns the trailing frames. Call once after the last Push().
  std::vector<FramePtr> Flush();

 private:
  // Converts the channel count the callers speak into the layout mask
  // MakeSwrContext expects, for counts that have a default layout.
  static uint64_t LayoutMask(int channels);

  SwrPtr swr_;
  int in_rate_{0};
  int in_channels_{0};
  AVSampleFormat in_fmt_{AV_SAMPLE_FMT_NONE};
  int out_rate_{0};
  int out_channels_{0};
  AVSampleFormat out_fmt_{AV_SAMPLE_FMT_NONE};
};

}  // namespace avbase::media::ffmpeg

#endif  // AVBASE_MEDIA_FFMPEG_AUDIO_CONVERT_H_
