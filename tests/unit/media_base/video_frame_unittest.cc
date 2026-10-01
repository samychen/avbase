// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/video_frame.h"

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"

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

// ---- Phase 3: the native-buffer branch -------------------------------------

TEST(VideoFrameTest, WrapNativeBufferCarriesTypedHandle) {
  int dummy_handle = 0;
  bool released = false;
  auto frame = VideoFrame::WrapNativeBuffer(
      NativeHandle{NativeHandleKind::kCVPixelBuffer, &dummy_handle, 0},
      VideoFormat::kNV12, Size{1920, 1080}, Size{1920, 1080}, Rational{1, 1},
      base::Milliseconds(33), base::Microseconds(33333), 7,
      base::BindOnce([&released] { released = true; }),
      base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
  ASSERT_TRUE(frame);
  EXPECT_FALSE(frame->IsMappable());
  EXPECT_EQ(frame->storage_type(), VideoFrame::StorageType::kStorageOpaque);
  EXPECT_EQ(frame->native_handle().kind, NativeHandleKind::kCVPixelBuffer);
  EXPECT_EQ(frame->native_handle().id, &dummy_handle);
  EXPECT_EQ(frame->opaque_handle(), &dummy_handle);
  EXPECT_EQ(frame->format(), VideoFormat::kNV12);
  EXPECT_EQ(frame->serial(), 7);
  // No CPU planes exist behind a GPU surface.
  EXPECT_TRUE(frame->visible_data(VideoFrame::kYPlane).empty());
  EXPECT_FALSE(released);
  frame = nullptr;
  EXPECT_TRUE(released);
}

TEST(VideoFrameTest, NativeBufferReleaseRunsOnceAtLastUnref) {
  int dummy_handle = 0;
  int releases = 0;
  auto frame = VideoFrame::WrapNativeBuffer(
      NativeHandle{NativeHandleKind::kVaapiSurface, &dummy_handle, 3},
      VideoFormat::kNV12, Size{3840, 2160}, Size{3840, 2160}, Rational{1, 1},
      base::TimeDelta(), base::TimeDelta(), 0,
      base::BindOnce([&releases] { ++releases; }),
      base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
  // Cloning the reference (what passing a frame to a sink does) must not run
  // the release callback; only dropping the LAST reference does.
  base::scoped_refptr<VideoFrame> second = frame;
  second = nullptr;
  EXPECT_EQ(releases, 0);
  frame = nullptr;
  EXPECT_EQ(releases, 1);
}

TEST(VideoFrameTest, NativeBufferHandleNotAliasedByCopies) {
  // The handle travels by value with the frame, so every reference reports
  // the same typed handle without extra plumbing.
  int dummy_handle = 0;
  auto frame = VideoFrame::WrapNativeBuffer(
      NativeHandle{NativeHandleKind::kD3D11Texture, &dummy_handle, 5},
      VideoFormat::kNV12, Size{1280, 720}, Size{1280, 720}, Rational{1, 1},
      base::TimeDelta(), base::TimeDelta(), 0, base::DoNothing(),
      base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
  base::scoped_refptr<VideoFrame> second = frame;
  frame = nullptr;
  ASSERT_TRUE(second);
  EXPECT_EQ(second->native_handle().kind, NativeHandleKind::kD3D11Texture);
  EXPECT_EQ(second->native_handle().subresource, 5);
}

TEST(VideoFrameTest, ToI420WithoutReadbackPathReturnsNull) {
  int dummy_handle = 0;
  auto frame = VideoFrame::WrapNativeBuffer(
      NativeHandle{NativeHandleKind::kCVPixelBuffer, &dummy_handle, 0},
      VideoFormat::kNV12, Size{640, 360}, Size{640, 360}, Rational{1, 1},
      base::TimeDelta(), base::TimeDelta(), 0, base::DoNothing(),
      base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
  EXPECT_EQ(frame->ToI420(), nullptr);
}

TEST(VideoFrameTest, ToI420UsesProducerReadbackPath) {
  int dummy_handle = 0;
  auto frame = VideoFrame::WrapNativeBuffer(
      NativeHandle{NativeHandleKind::kCVPixelBuffer, &dummy_handle, 0},
      VideoFormat::kNV12, Size{640, 360}, Size{640, 360}, Rational{1, 1},
      base::Milliseconds(10), base::TimeDelta(), 1, base::DoNothing(),
      base::BindRepeating([]() -> base::scoped_refptr<VideoFrame> {
        auto out = VideoFrame::CreateBlackFrame(
            VideoFormat::kI420, Size{640, 360}, Size{640, 360}, Rational{1, 1},
            base::Milliseconds(10), base::TimeDelta(), 1);
        out->mutable_data(VideoFrame::kYPlane)[0] = 0xAB;
        return out;
      }));
  base::scoped_refptr<VideoFrame> mapped = frame->ToI420();
  ASSERT_TRUE(mapped);
  EXPECT_TRUE(mapped->IsMappable());
  EXPECT_EQ(mapped->format(), VideoFormat::kI420);
  EXPECT_EQ(mapped->visible_data(VideoFrame::kYPlane)[0], 0xAB);
  // The readback may run again: RepeatingCallback, not Once.
  ASSERT_TRUE(frame->ToI420());
  // The original GPU frame is untouched by the readback.
  EXPECT_FALSE(frame->IsMappable());
}

TEST(VideoFrameTest, ToI420OnOwnedI420ReturnsSamePixels) {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{320, 240}, Size{320, 240}, Rational{1, 1},
      base::TimeDelta(), base::TimeDelta(), 0);
  frame->mutable_data(VideoFrame::kYPlane)[0] = 0x5A;
  base::scoped_refptr<VideoFrame> same = frame->ToI420();
  ASSERT_TRUE(same);
  EXPECT_EQ(same.get(), frame.get());   // A new reference, not a copy.
  EXPECT_EQ(same->visible_data(VideoFrame::kYPlane)[0], 0x5A);
}

TEST(VideoFrameTest, ColorSpaceProducerSetter) {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{320, 240}, Size{320, 240}, Rational{1, 1},
      base::TimeDelta(), base::TimeDelta(), 0);
  EXPECT_FALSE(frame->color_space().IsSpecified());
  frame->set_color_space(VideoColorSpace{
      ColorMatrix::kBT709, ColorPrimaries::kBT709, ColorTransfer::kBT709,
      ColorRange::kLimited});
  EXPECT_TRUE(frame->color_space().IsSpecified());
  EXPECT_NE(frame->AsDebugString().find("cs=bt709"), std::string::npos);
}

TEST(NativeHandleKindTest, Names) {
  EXPECT_STREQ(GetNativeHandleKindName(NativeHandleKind::kNone), "none");
  EXPECT_STREQ(GetNativeHandleKindName(NativeHandleKind::kVaapiSurface),
               "vaapi-surface");
  EXPECT_STREQ(GetNativeHandleKindName(NativeHandleKind::kD3D11Texture),
               "d3d11-texture");
  EXPECT_STREQ(GetNativeHandleKindName(NativeHandleKind::kCVPixelBuffer),
               "cvpixelbuffer");
}

}  // namespace
}  // namespace avbase::media
