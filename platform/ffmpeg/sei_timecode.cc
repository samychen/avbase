// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/ffmpeg/sei_timecode.h"

#include <cstdio>
#include <cstring>

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

bool WriteTimecodeToFrame(AVFrame* frame, const S12mTimecode& tc) {
  if (!frame) {
    return false;
  }
  // ST 12-1 payload: word 0 = count (4 = HH/MM/SS/FF), then 4 words.
  uint32_t words[5] = {0};
  words[0] = 4;
  words[1] = static_cast<uint32_t>(tc.hours);
  words[2] = static_cast<uint32_t>(tc.minutes);
  words[3] = static_cast<uint32_t>(tc.seconds);
  words[4] = static_cast<uint32_t>(tc.frames);

  // Remove existing timecode side data if present.
  av_frame_remove_side_data(frame, AV_FRAME_DATA_S12M_TIMECODE);

  AVFrameSideData* sd =
      av_frame_new_side_data(frame, AV_FRAME_DATA_S12M_TIMECODE, sizeof(words));
  if (!sd) {
    return false;
  }
  std::memcpy(sd->data, words, sizeof(words));
  return true;
}

}  // namespace avbase::platform::ffmpeg
