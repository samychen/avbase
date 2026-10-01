// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/video_color_space.h"

#include "gtest/gtest.h"

namespace avbase::media {
namespace {

VideoColorSpace Guess(int height, bool hdr, int width = 1920) {
  return GuessColorSpaceFallback(width, height, hdr);
}

// ---- The size-class ladder --------------------------------------------------

TEST(GuessColorSpaceFallbackTest, HdContentIsBt709) {
  // The common real-world repair: an HD stream whose hwaccel frame came back
  // with all-unspecified colour tags.
  const VideoColorSpace cs = Guess(1080, false);
  EXPECT_EQ(cs.matrix, ColorMatrix::kBT709);
  EXPECT_EQ(cs.primaries, ColorPrimaries::kBT709);
  EXPECT_EQ(cs.transfer, ColorTransfer::kBT709);
  EXPECT_EQ(cs.range, ColorRange::kLimited);
}

TEST(GuessColorSpaceFallbackTest, SdContentIsBt601) {
  const VideoColorSpace pal = Guess(576, false);
  EXPECT_EQ(pal.matrix, ColorMatrix::kSMPTE170M);
  EXPECT_EQ(pal.primaries, ColorPrimaries::kSMPTE170M);
  const VideoColorSpace ntsc = Guess(480, false);
  EXPECT_EQ(ntsc.matrix, ColorMatrix::kSMPTE170M);
  EXPECT_EQ(ntsc.primaries, ColorPrimaries::kSMPTE170M);
}

TEST(GuessColorSpaceFallbackTest, BoundaryHeight599And600) {
  // 599 stays SD, 600 is the first HD row. The table brackets are inclusive;
  // these two lines pin the seam.
  EXPECT_EQ(Guess(599, false).matrix,
            ColorMatrix::kSMPTE170M);
  EXPECT_EQ(Guess(600, false).matrix, ColorMatrix::kBT709);
}

TEST(GuessColorSpaceFallbackTest, HdrOverridesEverything) {
  const VideoColorSpace cs = Guess(2160, true);
  EXPECT_EQ(cs.matrix, ColorMatrix::kBT2020Ncl);
  EXPECT_EQ(cs.primaries, ColorPrimaries::kBT2020);
  EXPECT_EQ(cs.transfer, ColorTransfer::kSMPTE2084);
  // And HDR wins even at SD sizes: a container claiming PQ is BT.2020 by
  // definition of its transfer.
  const VideoColorSpace odd = Guess(480, true);
  EXPECT_EQ(odd.transfer, ColorTransfer::kSMPTE2084);
  EXPECT_EQ(odd.primaries, ColorPrimaries::kBT2020);
}

TEST(GuessColorSpaceFallbackTest, ResultIsNeverPartiallySpecified) {
  // Every row specifies the FULL tuple (matrix/primaries/transfer/range) or
  // nothing -- a half-tagged frame is worse than an untagged one because it
  // looks authoritative.
  for (const int height : {240, 480, 576, 599, 600, 720, 1080, 2160, 4320}) {
    const VideoColorSpace cs = Guess(height, false);
    EXPECT_TRUE(cs.IsSpecified()) << "height " << height;
    EXPECT_EQ(cs.range, ColorRange::kLimited) << "height " << height;
  }
}

TEST(GuessColorSpaceFallbackTest, FourKHdRowCoversUhd) {
  EXPECT_EQ(Guess(2160, false).matrix,
            ColorMatrix::kBT709);
  EXPECT_EQ(Guess(4320, false).matrix, ColorMatrix::kBT709);
}

// ---- The value types --------------------------------------------------------

TEST(VideoColorSpaceTest, Names) {
  EXPECT_STREQ(GetColorMatrixName(ColorMatrix::kBT709), "bt709");
  EXPECT_STREQ(GetColorMatrixName(ColorMatrix::kSMPTE170M), "smpte170m");
  EXPECT_STREQ(GetColorTransferName(ColorTransfer::kSMPTE2084), "smpte2084");
  EXPECT_STREQ(GetColorPrimariesName(ColorPrimaries::kBT2020), "bt2020");
  EXPECT_STREQ(GetColorRangeName(ColorRange::kLimited), "limited");
}

TEST(VideoColorSpaceTest, IsSpecifiedNeedsAnyAxis) {
  EXPECT_FALSE((VideoColorSpace{}).IsSpecified());
  EXPECT_TRUE((VideoColorSpace{ColorMatrix::kBT709, ColorPrimaries::kUnknown,
                               ColorTransfer::kUnknown, ColorRange::kUnknown})
                  .IsSpecified());
}

TEST(VideoColorSpaceTest, DebugStringIsSlashSeparated) {
  const VideoColorSpace cs{ColorMatrix::kBT2020Ncl, ColorPrimaries::kBT2020,
                           ColorTransfer::kSMPTE2084, ColorRange::kLimited};
  EXPECT_EQ(cs.AsDebugString(), "bt2020ncl/bt2020/smpte2084/limited");
}

}  // namespace
}  // namespace avbase::media
