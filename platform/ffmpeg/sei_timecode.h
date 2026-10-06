// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_FFMPEG_SEI_TIMECODE_H_
#define AVBASE_PLATFORM_FFMPEG_SEI_TIMECODE_H_

#include <optional>
#include <string>

#include "platform/ffmpeg/av_includes.h"

namespace avbase::platform::ffmpeg {

// SMPTE ST 12-1 timecode, as FFmpeg surfaces it from the H.264/MPEG-2 SEI
// timecode message (AV_FRAME_DATA_S12M_TIMECODE side data). MediaComponent
// carried a bespoke "frame_cnt:<n> time_code:<...>" string convention for the
// same alignment job; the standard payload is what a player should read.
struct S12mTimecode {
  int hours = 0;
  int minutes = 0;
  int seconds = 0;
  int frames = 0;

  // "HH:MM:SS:FF", zero-padded.
  std::string ToString() const;
};

// Reads the S12M timecode side data from |frame|; nullopt when the frame
// carries none or the payload is malformed (wrong count or out-of-range
// fields -- a malformed message must not surface as garbage timecode).
std::optional<S12mTimecode> TimecodeFromFrame(const AVFrame& frame);

}  // namespace avbase::platform::ffmpeg

#endif  // AVBASE_PLATFORM_FFMPEG_SEI_TIMECODE_H_
