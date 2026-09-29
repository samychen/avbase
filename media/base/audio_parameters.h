// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/audio_parameters.h` (BSD-3-Clause).

#ifndef IJKPP_MEDIA_BASE_AUDIO_PARAMETERS_H_
#define IJKPP_MEDIA_BASE_AUDIO_PARAMETERS_H_

#include <stdint.h>

#include "base/time/time.h"
#include "media/media_export.h"

namespace ijkpp::media {

enum class SampleFormat {
  kUnknown = 0, kU8, kS16, kS32, kF32, kS16P, kS32P, kF32P,
};
enum class ChannelLayout {
  kNone = 0, kMono, kStereo, k2_1, kSurround, k4_0, kQuad, k5_0,
  k5_1, k7_1, kDiscrete,
};

IJKPP_MEDIA_EXPORT const char* GetSampleFormatName(SampleFormat format);
IJKPP_MEDIA_EXPORT const char* GetChannelLayoutName(ChannelLayout layout);
IJKPP_MEDIA_EXPORT int ChannelLayoutToChannelCount(ChannelLayout layout);
IJKPP_MEDIA_EXPORT int SampleFormatBytesPerChannel(SampleFormat format);

// Decodes one sample at |src| into a float in [-1, 1]. Returns 0 for unknown
// formats rather than trapping: a malformed stream must degrade to silence, not
// crash the audio thread (which would take the whole player down).
IJKPP_MEDIA_EXPORT float DecodeSample(const uint8_t* src, SampleFormat format);

class IJKPP_MEDIA_EXPORT AudioParameters {
 public:
  AudioParameters() = default;
  AudioParameters(ChannelLayout layout, SampleFormat format, int sample_rate,
                  int frames_per_buffer)
      : channel_layout_(layout), sample_format_(format),
        sample_rate_(sample_rate), frames_per_buffer_(frames_per_buffer) {}

  ChannelLayout channel_layout() const { return channel_layout_; }
  SampleFormat sample_format() const { return sample_format_; }
  int sample_rate() const { return sample_rate_; }
  int frames_per_buffer() const { return frames_per_buffer_; }
  int channels() const { return ChannelLayoutToChannelCount(channel_layout_); }
  int bytes_per_frame() const {
    return channels() * SampleFormatBytesPerChannel(sample_format_);
  }
  base::TimeDelta buffer_duration() const {
    return sample_rate_ > 0
               ? base::Microseconds(static_cast<int64_t>(frames_per_buffer_) *
                                    1000000 / sample_rate_)
               : base::TimeDelta();
  }
  bool is_valid() const {
    return sample_rate_ > 0 && frames_per_buffer_ > 0 && bytes_per_frame() > 0;
  }
  friend bool operator==(const AudioParameters&, const AudioParameters&) = default;

 private:
  ChannelLayout channel_layout_{ChannelLayout::kStereo};
  SampleFormat sample_format_{SampleFormat::kS16};
  int sample_rate_{48000};
  int frames_per_buffer_{1024};
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_AUDIO_PARAMETERS_H_
