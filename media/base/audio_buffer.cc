// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/audio_buffer.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "base/check.h"

namespace ijkpp::media {

AudioBuffer::AudioBuffer() = default;
AudioBuffer::~AudioBuffer() = default;

// static
base::scoped_refptr<AudioBuffer> AudioBuffer::Create(
    SampleFormat sample_format, ChannelLayout channel_layout, int channels,
    int sample_rate, int frame_count, base::TimeDelta timestamp,
    base::TimeDelta duration, int32_t serial, std::vector<uint8_t> data) {
  base::scoped_refptr<AudioBuffer> buffer(new AudioBuffer());
  buffer->sample_format_ = sample_format;
  buffer->channel_layout_ = channel_layout;
  buffer->channels_ = channels;
  buffer->sample_rate_ = sample_rate;
  buffer->frame_count_ = frame_count;
  buffer->timestamp_ = timestamp;
  buffer->duration_ = duration;
  buffer->serial_ = serial;
  buffer->data_ = std::move(data);
  return buffer;
}

// static
base::scoped_refptr<AudioBuffer> AudioBuffer::CreateEOSBuffer() {
  base::scoped_refptr<AudioBuffer> buffer(new AudioBuffer());
  buffer->is_eos_ = true;
  return buffer;
}

bool AudioBuffer::is_planar() const {
  switch (sample_format_) {
    case SampleFormat::kS16P:
    case SampleFormat::kS32P:
    case SampleFormat::kF32P:
      return true;
    default:
      return false;
  }
}

std::span<const uint8_t> AudioBuffer::channel_data(int channel) const {
  if (channel < 0 || channel >= channels_ || data_.empty()) {
    return {};
  }
  if (!is_planar()) {
    // Interleaved: every channel lives in the same span, strided by the frame.
    return data_;
  }
  const size_t per_channel = data_.size() / static_cast<size_t>(channels_);
  const size_t offset = per_channel * static_cast<size_t>(channel);
  if (offset >= data_.size()) {
    return {};
  }
  return std::span<const uint8_t>(data_.data() + offset,
                                  std::min(per_channel, data_.size() - offset));
}

int AudioBuffer::ReadFrames(int frames, int offset_frames, AudioBus* dest) const {
  CHECK(dest);
  if (is_eos_ || data_.empty() || channels_ <= 0) {
    return 0;
  }
  const int available = frame_count_ - offset_frames;
  const int count = std::min({frames, available, dest->frames()});
  if (count <= 0) {
    return 0;
  }
  const int out_channels = std::min(channels_, dest->channels());
  const int bytes_per_sample_int = SampleFormatBytesPerChannel(sample_format_);
  if (bytes_per_sample_int <= 0) {
    return 0;
  }
  // Held as size_t from here on: every use is pointer arithmetic, and keeping it
  // signed would make each one an implicit int -> size_t conversion that
  // -Wsign-conversion (debug preset, -Werror) rejects.
  const size_t bytes_per_sample = static_cast<size_t>(bytes_per_sample_int);

  for (int ch = 0; ch < out_channels; ++ch) {
    float* out = dest->channel(ch);
    if (is_planar()) {
      const std::span<const uint8_t> src = channel_data(ch);
      const size_t base =
          static_cast<size_t>(offset_frames) * bytes_per_sample;
      for (int f = 0; f < count; ++f) {
        const size_t i = base + static_cast<size_t>(f) * bytes_per_sample;
        if (i + bytes_per_sample > src.size()) {
          out[f] = 0.0f;
          continue;
        }
        out[f] = DecodeSample(src.data() + i, sample_format_);
      }
    } else {
      const size_t frame_bytes = bytes_per_sample * static_cast<size_t>(channels_);
      const size_t base = static_cast<size_t>(offset_frames) * frame_bytes +
                          static_cast<size_t>(ch) * bytes_per_sample;
      for (int f = 0; f < count; ++f) {
        const size_t i = base + static_cast<size_t>(f) * frame_bytes;
        if (i + bytes_per_sample > data_.size()) {
          out[f] = 0.0f;
          continue;
        }
        out[f] = DecodeSample(data_.data() + i, sample_format_);
      }
    }
  }
  // Fill any remaining output channels with silence rather than leaving stale
  // data from a previous buffer, which would be audible as a stuck tone.
  for (int ch = out_channels; ch < dest->channels(); ++ch) {
    std::fill(dest->channel(ch), dest->channel(ch) + count, 0.0f);
  }
  return count;
}

std::string AudioBuffer::AsDebugString() const {
  if (is_eos_) {
    return "AudioBuffer(EOS)";
  }
  std::string out = "AudioBuffer(";
  out += GetSampleFormatName(sample_format_);
  out += " ";
  out += GetChannelLayoutName(channel_layout_);
  out += " " + std::to_string(sample_rate_) + "Hz";
  out += " frames=" + std::to_string(frame_count_);
  out += " ts=" + std::to_string(timestamp_.InMicroseconds()) + "us";
  out += " serial=" + std::to_string(serial_);
  out += ")";
  return out;
}

}  // namespace ijkpp::media
