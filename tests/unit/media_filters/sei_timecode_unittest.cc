// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/ffmpeg/sei_timecode.h"

#include <gtest/gtest.h>

#include "platform/ffmpeg/av_includes.h"
#include "platform/ffmpeg/compat.h"

namespace avbase::platform::ffmpeg {
namespace {

// Builds a frame carrying an S12M timecode side payload: word 0 = field
// count, then HH/MM/SS/FF (ST 12-1 layout FFmpeg uses).
FramePtr FrameWithTimecode(int hh, int mm, int ss, int ff) {
  FramePtr frame(av_frame_alloc());
  const size_t bytes = 5 * sizeof(uint32_t);
  auto* data = reinterpret_cast<uint32_t*>(
      av_frame_new_side_data(frame.get(), AV_FRAME_DATA_S12M_TIMECODE, bytes)
          ->data);
  data[0] = 4;
  data[1] = static_cast<uint32_t>(hh);
  data[2] = static_cast<uint32_t>(mm);
  data[3] = static_cast<uint32_t>(ss);
  data[4] = static_cast<uint32_t>(ff);
  return frame;
}

TEST(SeiTimecodeTest, ReadsWellFormedPayload) {
  const auto tc = TimecodeFromFrame(*FrameWithTimecode(13, 34, 56, 23));
  ASSERT_TRUE(tc.has_value());
  EXPECT_EQ(tc->ToString(), "13:34:56:23");
}

TEST(SeiTimecodeTest, RejectsMissingSideData) {
  FramePtr frame(av_frame_alloc());
  EXPECT_FALSE(TimecodeFromFrame(*frame).has_value());
}

TEST(SeiTimecodeTest, RejectsMalformedPayloads) {
  // Wrong field count.
  auto frame = FrameWithTimecode(0, 0, 0, 0);
  auto* sd = av_frame_get_side_data(frame.get(), AV_FRAME_DATA_S12M_TIMECODE);
  reinterpret_cast<uint32_t*>(sd->data)[0] = 2;  // drop-frame form
  EXPECT_FALSE(TimecodeFromFrame(*frame).has_value());

  // Out-of-range fields: minutes 61 is not a clock.
  auto bad = FrameWithTimecode(0, 61, 0, 0);
  EXPECT_FALSE(TimecodeFromFrame(*bad).has_value());
}

// E5: WriteTimecodeToFrame round-trips through TimecodeFromFrame.
TEST(SeiTimecodeTest, WriteAndReadBack) {
  FramePtr frame(av_frame_alloc());
  S12mTimecode tc{10, 20, 30, 15};
  EXPECT_TRUE(WriteTimecodeToFrame(frame.get(), tc));
  const auto read = TimecodeFromFrame(*frame);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->ToString(), "10:20:30:15");
}

// E5: Writing a new timecode replaces the existing one.
TEST(SeiTimecodeTest, WriteReplacesExisting) {
  FramePtr frame(av_frame_alloc());
  S12mTimecode tc1{1, 2, 3, 4};
  EXPECT_TRUE(WriteTimecodeToFrame(frame.get(), tc1));
  S12mTimecode tc2{5, 6, 7, 8};
  EXPECT_TRUE(WriteTimecodeToFrame(frame.get(), tc2));
  const auto read = TimecodeFromFrame(*frame);
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->ToString(), "05:06:07:08");
}

}  // namespace
}  // namespace avbase::platform::ffmpeg
