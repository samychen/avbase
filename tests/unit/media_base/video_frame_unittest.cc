// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/video_frame.h"

#include "gtest/gtest.h"

namespace avbase::media {
namespace {

TEST(VideoFormatTest, PlaneCounts) {
  EXPECT_EQ(VideoFormatPlaneCount(VideoFormat::kI420), 3);
  EXPECT_EQ(VideoFormatPlaneCount(VideoFormat::kYV12), 3);
  EXPECT_EQ(VideoFormatPlaneCount(VideoFormat::kNV12), 2);
  EXPECT_EQ(VideoFormatPlaneCount(VideoFormat::kARGB), 1);
  EXPECT_EQ(VideoFormatPlaneCount(VideoFormat::kRGB24), 1);
  EXPECT_EQ(VideoFormatPlaneCount(VideoFormat::kUnknown), 0);
}

TEST(VideoFormatTest, Names) {
  EXPECT_STREQ(GetVideoFormatName(VideoFormat::kI420), "I420");
  EXPECT_STREQ(GetVideoFormatName(VideoFormat::kNV12), "NV12");
  EXPECT_STREQ(GetVideoFormatName(VideoFormat::kP010), "P010");
}

TEST(VideoFrameTest, CreateBlackFramePopulatesMetadata) {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{1920, 1080}, Size{1920, 1080}, Rational{1, 1},
      base::Seconds(2), base::Microseconds(33333), /*serial=*/7);
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->format(), VideoFormat::kI420);
  EXPECT_EQ(frame->storage_type(), VideoFrame::StorageType::kStorageOwned);
  EXPECT_EQ(frame->coded_size(), (Size{1920, 1080}));
  EXPECT_EQ(frame->natural_size(), (Size{1920, 1080}));
  EXPECT_EQ(frame->timestamp(), base::Seconds(2));
  EXPECT_EQ(frame->duration(), base::Microseconds(33333));
  EXPECT_EQ(frame->serial(), 7);
  EXPECT_TRUE(frame->IsMappable());
}

TEST(VideoFrameTest, PlanesAreAllocatedAndZeroFilled) {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{64, 64}, Size{64, 64}, Rational{1, 1},
      base::TimeDelta(), base::TimeDelta(), 0);
  const auto y = frame->visible_data(VideoFrame::kYPlane);
  const auto u = frame->visible_data(VideoFrame::kUPlane);
  const auto v = frame->visible_data(VideoFrame::kVPlane);
  ASSERT_FALSE(y.empty());
  ASSERT_FALSE(u.empty());
  ASSERT_FALSE(v.empty());
  EXPECT_GE(y.size(), 64u * 64u);
  EXPECT_GE(u.size(), 32u * 32u);
  EXPECT_GE(frame->stride(VideoFrame::kYPlane), 64);
  for (uint8_t byte : y) {
    ASSERT_EQ(byte, 0);
    break;   // calloc guarantees the whole allocation is zeroed.
  }
}

TEST(VideoFrameTest, SubsampledChromaIsHalfSize) {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{1920, 1080}, Size{1920, 1080}, Rational{1, 1},
      base::TimeDelta(), base::TimeDelta(), 0);
  EXPECT_GE(frame->allocated_size(VideoFrame::kYPlane), 1920u * 1080u);
  EXPECT_GE(frame->allocated_size(VideoFrame::kUPlane), 960u * 540u);
}

TEST(VideoFrameTest, NV12HasTwoPlanes) {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kNV12, Size{64, 64}, Size{64, 64}, Rational{1, 1},
      base::TimeDelta(), base::TimeDelta(), 0);
  EXPECT_FALSE(frame->visible_data(VideoFrame::kYPlane).empty());
  EXPECT_FALSE(frame->visible_data(VideoFrame::kUPlane).empty());
  EXPECT_TRUE(frame->visible_data(VideoFrame::kVPlane).empty());
}

TEST(VideoFrameTest, RefCountingSharesOneAllocation) {
  auto a = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{16, 16}, Size{16, 16}, Rational{1, 1},
      base::TimeDelta(), base::TimeDelta(), 0);
  const uint8_t* y_ptr = a->visible_data(VideoFrame::kYPlane).data();
  {
    base::scoped_refptr<VideoFrame> b = a;
    EXPECT_EQ(b->visible_data(VideoFrame::kYPlane).data(), y_ptr);
  }
  EXPECT_EQ(a->visible_data(VideoFrame::kYPlane).data(), y_ptr);
}

TEST(VideoFrameTest, SarAndRotationArePreserved) {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{720, 576}, Size{1024, 576}, Rational{64, 45},
      base::TimeDelta(), base::TimeDelta(), 0);
  EXPECT_EQ(frame->sar(), (Rational{64, 45}));
  EXPECT_EQ(frame->natural_size(), (Size{1024, 576}));
  EXPECT_EQ(frame->rotation(), 0);
}

TEST(VideoFrameTest, DebugStringContainsKeyFields) {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kNV12, Size{1920, 1080}, Size{1920, 1080}, Rational{1, 1},
      base::Milliseconds(1500), base::Microseconds(33333), 3);
  const std::string s = frame->AsDebugString();
  EXPECT_NE(s.find("NV12"), std::string::npos);
  EXPECT_NE(s.find("1920x1080"), std::string::npos);
  EXPECT_NE(s.find("serial=3"), std::string::npos);
}

TEST(SizeTest, Helpers) {
  // Named locals rather than braced temporaries: a comma inside braces would be
  // parsed as a gtest macro argument separator.
  const Size empty{0, 0};
  const Size one{1, 1};
  const Size area{4, 5};
  EXPECT_TRUE(empty.IsEmpty());
  EXPECT_FALSE(one.IsEmpty());
  EXPECT_EQ(area.GetArea(), 20);
}

}  // namespace
}  // namespace avbase::media
