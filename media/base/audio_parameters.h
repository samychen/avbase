// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/audio_parameters.h` (BSD-3-Clause).

#ifndef AVBASE_MEDIA_BASE_AUDIO_PARAMETERS_H_
#define AVBASE_MEDIA_BASE_AUDIO_PARAMETERS_H_

#include <stdint.h>

#include "base/time/time.h"
#include "media/media_export.h"

namespace avbase::media {

enum class SampleFormat {
  kUnknown = 0,
  kU8,
  kS16,
  kS32,
  kF32,
  kS16P,
  kS32P,
  kF32P,
};
enum class ChannelLayout {
  kNone = 0,
  kMono,
  kStereo,
  k2_1,
  kSurround,
  k4_0,
  kQuad,
  k5_0,
  k5_1,
  k7_1,
  kDiscrete,
};

AVBASE_MEDIA_EXPORT const char* GetSampleFormatName(SampleFormat format);
AVBASE_MEDIA_EXPORT const char* GetChannelLayoutName(ChannelLayout layout);
AVBASE_MEDIA_EXPORT int ChannelLayoutToChannelCount(ChannelLayout layout);
AVBASE_MEDIA_EXPORT int SampleFormatBytesPerChannel(SampleFormat format);

// Decodes one sample at |src| into a float in [-1, 1]. Returns 0 for unknown
// formats rather than trapping: a malformed stream must degrade to silence, not
// crash the audio thread (which would take the whole player down).
AVBASE_MEDIA_EXPORT float DecodeSample(const uint8_t* src, SampleFormat format);

class AVBASE_MEDIA_EXPORT AudioParameters {
 public:
  AudioParameters() = default;
  // |channels| defaults to 0, which means "take it from |layout|". Pass it
  // explicitly when the layout cannot carry the count: ChannelLayout::kDiscrete
  // is what the demuxer falls back to for any channel count that has no named
  // layout (3, 4, 5, 7, ...), and ChannelLayoutToChannelCount() answers 0 for
  // it. Deriving from the layout there is how a 4-channel file reached the
  // audio renderer as "0 channels" and aborted on its CHECK.
  AudioParameters(ChannelLayout layout, SampleFormat format, int sample_rate,
                  int frames_per_buffer, int channels = 0)
      : channel_layout_(layout),
        sample_format_(format),
        sample_rate_(sample_rate),
        frames_per_buffer_(frames_per_buffer),
        channels_(channels) {}

  ChannelLayout channel_layout() const { return channel_layout_; }
  SampleFormat sample_format() const { return sample_format_; }
  int sample_rate() const { return sample_rate_; }
  int frames_per_buffer() const { return frames_per_buffer_; }
  // The explicit count wins when one was given; otherwise it comes from the
  // layout, which is right for every named layout and 0 for kDiscrete/kNone.
  int channels() const {
    return channels_ > 0 ? channels_
                         : ChannelLayoutToChannelCount(channel_layout_);
  }
  int bytes_per_frame() const {
    return channels() * SampleFormatBytesPerChannel(sample_format_);
  }
  base::TimeDelta buffer_duration() const {
    return sample_rate_ > 0
               ? base::Microseconds(static_cast<int64_t>(frames_per_buffer_) *
                                    1000000 / sample_rate_)
               : base::TimeDelta();
  }
  // The sample format is deliberately NOT part of validity. It describes what
  // the SOURCE declares, and a container is allowed not to declare one: a bare
  // .ac3, an Ogg/Vorbis or an MPEG-PS with mp2 leaves
  // AVCodecParameters::format at AV_SAMPLE_FMT_NONE, which the demuxer reports
  // as SampleFormat::kUnknown. AudioDecoderConfig::IsValidConfig() already
  // accepts that, and nothing on the render path reads this field for sizing --
  // the decoder reports the real format with every decoded buffer. Requiring it
  // here is what turned "the container did not say" into a process abort on a
  // perfectly playable file.
  bool is_valid() const {
    return sample_rate_ > 0 && frames_per_buffer_ > 0 && channels() > 0;
  }
  friend bool operator==(const AudioParameters&,
                         const AudioParameters&) = default;

 private:
  ChannelLayout channel_layout_{ChannelLayout::kStereo};
  SampleFormat sample_format_{SampleFormat::kS16};
  int sample_rate_{48000};
  int frames_per_buffer_{1024};
  int channels_{0};
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_AUDIO_PARAMETERS_H_
