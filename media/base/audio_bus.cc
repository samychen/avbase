// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/audio_bus.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "base/check.h"

namespace avbase::media {

AudioBus::AudioBus(int channels, int frames)
    : channels_(channels),
      frames_(frames),
      // Callers pass int because the audio pipeline counts channels and frames
      // as int throughout; the cast is safe because CHECK_GT below rejects
      // anything non-positive, and the vector wants an unsigned size.
      data_(static_cast<size_t>(channels)) {
  CHECK_GT(channels, 0);
  CHECK_GT(frames, 0);
  for (auto& ch : data_) {
    ch.resize(static_cast<size_t>(frames), 0.0f);
  }
}

AudioBus::~AudioBus() = default;

// static
std::unique_ptr<AudioBus> AudioBus::Create(int channels, int frames) {
  return std::unique_ptr<AudioBus>(new AudioBus(channels, frames));
}

float* AudioBus::channel(int ch) {
  DCHECK_GE(ch, 0);
  DCHECK_LT(ch, channels_);
  return data_[static_cast<size_t>(ch)].data();
}

const float* AudioBus::channel(int ch) const {
  DCHECK_GE(ch, 0);
  DCHECK_LT(ch, channels_);
  return data_[static_cast<size_t>(ch)].data();
}

void AudioBus::Zero() {
  for (auto& ch : data_) {
    std::fill(ch.begin(), ch.end(), 0.0f);
  }
}

void AudioBus::Scale(float volume) {
  for (auto& ch : data_) {
    for (float& sample : ch) {
      sample *= volume;
    }
  }
}

void AudioBus::CopyTo(AudioBus* dest) const {
  CopyPartialTo(std::min(frames_, dest->frames()), dest);
}

void AudioBus::CopyPartialTo(int frames, AudioBus* dest) const {
  DCHECK(dest);
  const int n = std::min({frames, frames_, dest->frames()});
  const int ch = std::min(channels_, dest->channels());
  for (int c = 0; c < ch; ++c) {
    std::memcpy(dest->channel(c), channel(c),
                static_cast<size_t>(n) * sizeof(float));
  }
}

int AudioBus::ToInterleavedS16(int frames, int16_t* dest) const {
  DCHECK(dest);
  const int n = std::min(frames, frames_);
  for (int f = 0; f < n; ++f) {
    for (int c = 0; c < channels_; ++c) {
      const float clamped = std::clamp(channel(c)[f], -1.0f, 1.0f);
      dest[static_cast<size_t>(f) * static_cast<size_t>(channels_) +
           static_cast<size_t>(c)] =
          static_cast<int16_t>(std::lround(clamped * 32767.0f));
    }
  }
  return n;
}

}  // namespace avbase::media
