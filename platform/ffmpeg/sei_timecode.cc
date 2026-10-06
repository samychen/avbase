// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/ffmpeg/sei_timecode.h"

#include <cstdio>

namespace avbase::platform::ffmpeg {

std::string S12mTimecode::ToString() const {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", hours, minutes,
                seconds, frames);
  return buf;
}

std::optional<S12mTimecode> TimecodeFromFrame(const AVFrame& frame) {
  const AVFrameSideData* data =
      av_frame_get_side_data(&frame, AV_FRAME_DATA_S12M_TIMECODE);
  if (!data || data->size < sizeof(uint32_t)) {
    return std::nullopt;
  }
  // ST 12-1 payload via FFmpeg: word 0 = number of fields that follow (2 =
  // drop-frame pairs, 4 = HH/MM/SS/FF), then the fields themselves.
  const auto* words = reinterpret_cast<const uint32_t*>(data->data);
  const size_t count = words[0];
  if (count != 4 || data->size < (1 + count) * sizeof(uint32_t)) {
    return std::nullopt;
  }
  S12mTimecode tc;
  tc.hours = static_cast<int>(words[1]);
  tc.minutes = static_cast<int>(words[2]);
  tc.seconds = static_cast<int>(words[3]);
  tc.frames = static_cast<int>(words[4]);
  if (tc.hours < 0 || tc.hours > 23 || tc.minutes < 0 || tc.minutes > 59 ||
      tc.seconds < 0 || tc.seconds > 59 || tc.frames < 0 || tc.frames > 99) {
    return std::nullopt;
  }
  return tc;
}

}  // namespace avbase::platform::ffmpeg
