// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/audio_buffer.h` (BSD-3-Clause).

#ifndef AVBASE_MEDIA_BASE_AUDIO_BUFFER_H_
#define AVBASE_MEDIA_BASE_AUDIO_BUFFER_H_

#include <stdint.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/media_constants.h"
#include "media/media_export.h"

namespace avbase::media {

// Decoded audio: one buffer of interleaved or planar samples.
//
// Replaces the AVFrame that ijkplayer's audio_thread() passes to
// audio_decode_frame(), where the frame's layout (planar vs interleaved, sample
// format, channel count) has to be re-derived at every use site. Here the
// layout is described once, at construction.
class AVBASE_MEDIA_EXPORT AudioBuffer
    : public base::RefCountedThreadSafe<AudioBuffer> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  // Builds a buffer owning |data| (already laid out per |sample_format|).
  static base::scoped_refptr<AudioBuffer> Create(
      SampleFormat sample_format, ChannelLayout channel_layout, int channels,
      int sample_rate, int frame_count, base::TimeDelta timestamp,
      base::TimeDelta duration, int32_t serial, std::vector<uint8_t> data);
  static base::scoped_refptr<AudioBuffer> CreateEOSBuffer();

  AudioBuffer(const AudioBuffer&) = delete;
  AudioBuffer& operator=(const AudioBuffer&) = delete;

  bool end_of_stream() const { return is_eos_; }

  base::TimeDelta timestamp() const { return timestamp_; }
  base::TimeDelta duration() const { return duration_; }
  int32_t serial() const { return serial_; }
  void set_serial(int32_t serial) { serial_ = serial; }

  SampleFormat sample_format() const { return sample_format_; }
  ChannelLayout channel_layout() const { return channel_layout_; }
  int channel_count() const { return channels_; }
  int sample_rate() const { return sample_rate_; }
  int frame_count() const { return frame_count_; }
  bool is_planar() const;

  size_t data_size() const { return data_.size(); }
  std::span<const uint8_t> data() const { return data_; }
  // For planar formats, the slice belonging to |channel|.
  std::span<const uint8_t> channel_data(int channel) const;

  // Converts into |dest| (float planar), which is what AudioRendererSink and
  // AudioRendererAlgorithm both consume. Handles s16/s32/f32 and their planar
  // variants. Returns the frames written.
  int ReadFrames(int frames, int offset_frames, AudioBus* dest) const;

  std::string AsDebugString() const;

 private:
  friend class base::RefCountedThreadSafe<AudioBuffer>;
  // Non-virtual per the rule in base/memory/ref_counted.h: no virtual members.
  ~AudioBuffer();
  AudioBuffer();

  SampleFormat sample_format_{SampleFormat::kUnknown};
  ChannelLayout channel_layout_{ChannelLayout::kNone};
  int channels_{0};
  int sample_rate_{0};
  int frame_count_{0};
  int32_t serial_{0};
  base::TimeDelta timestamp_{media::kNoTimestamp};
  base::TimeDelta duration_;
  bool is_eos_{false};
  std::vector<uint8_t> data_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_AUDIO_BUFFER_H_
