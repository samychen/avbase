// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/audio_bus.h` (BSD-3-Clause): deinterleaved
// float32 audio, which is the format AudioRendererSink::RenderCallback fills.

#ifndef IJKPP_MEDIA_BASE_AUDIO_BUS_H_
#define IJKPP_MEDIA_BASE_AUDIO_BUS_H_

#include <memory>
#include <vector>

#include "media/media_export.h"

namespace ijkpp::media {

class IJKPP_MEDIA_EXPORT AudioBus {
 public:
  AudioBus(const AudioBus&) = delete;
  AudioBus& operator=(const AudioBus&) = delete;
  ~AudioBus();

  static std::unique_ptr<AudioBus> Create(int channels, int frames);

  int channels() const { return channels_; }
  int frames() const { return frames_; }

  float* channel(int ch);
  const float* channel(int ch) const;

  void Zero();
  void Scale(float volume);
  void CopyTo(AudioBus* dest) const;
  void CopyPartialTo(int frames, AudioBus* dest) const;

  // Interleaves float planar data into |dest| as 16-bit signed PCM, which is
  // what ALSA and PulseAudio want by default. Returns the frames written.
  int ToInterleavedS16(int frames, int16_t* dest) const;

 private:
  AudioBus(int channels, int frames);
  int channels_;
  int frames_;
  std::vector<std::vector<float>> data_;
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_AUDIO_BUS_H_
