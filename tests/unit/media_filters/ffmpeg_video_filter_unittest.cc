// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_video_filter.h"

#include <gtest/gtest.h>

#include "media/base/video_frame.h"

namespace avbase::media {
namespace {

constexpr Size kSize{64, 48};

base::scoped_refptr<VideoFrame> MakeGradientFrame() {
  auto frame = VideoFrame::CreateBlackFrame(VideoFormat::kI420, kSize, kSize,
                                            Rational{1, 1}, base::TimeDelta(),
                                            base::Milliseconds(33), 0);
  for (int y = 0; y < 48; ++y) {
    for (int x = 0; x < 64; ++x) {
      frame->mutable_data(VideoFrame::kYPlane)[static_cast<size_t>(y) * 64 +
                                               static_cast<size_t>(x)] =
          static_cast<uint8_t>((x * 4) & 0xff);
    }
  }
  return frame;
}

TEST(VideoFilterTest, NullGraphPassesPixelsThrough) {
  FFmpegVideoFilter filter;
  ASSERT_TRUE(filter.Initialize("null", VideoFormat::kI420, kSize));
  auto in = MakeGradientFrame();
  base::scoped_refptr<VideoFrame> out;
  ASSERT_TRUE(filter.Process(in, &out));
  ASSERT_TRUE(out);
  EXPECT_EQ(0,
            std::memcmp(out->visible_data(VideoFrame::kYPlane).data(),
                        in->visible_data(VideoFrame::kYPlane).data(), 64 * 48));
}

TEST(VideoFilterTest, HflipMirrorsRows) {
  FFmpegVideoFilter filter;
  ASSERT_TRUE(filter.Initialize("hflip", VideoFormat::kI420, kSize));
  auto in = MakeGradientFrame();
  base::scoped_refptr<VideoFrame> out;
  ASSERT_TRUE(filter.Process(in, &out));
  ASSERT_TRUE(out);
  const auto* src = in->visible_data(VideoFrame::kYPlane).data();
  const auto* dst = out->visible_data(VideoFrame::kYPlane).data();
  // dst(x, y) == src(w-1-x, y) is what a horizontal flip means.
  EXPECT_EQ(dst[0], src[63]);
  EXPECT_EQ(dst[32], src[31]);
  EXPECT_EQ(dst[63], src[0]);
  EXPECT_EQ(dst[48 * 64], src[48 * 64 + 63]);
}

TEST(VideoFilterTest, UnknownFilterFailsToInitialize) {
  FFmpegVideoFilter filter;
  EXPECT_FALSE(
      filter.Initialize("definitely_not_a_filter", VideoFormat::kI420, kSize));
}

}  // namespace
}  // namespace avbase::media
