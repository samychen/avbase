// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_video_filter.h"

#include <gtest/gtest.h>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
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

// A hardware-decoded frame has no CPU planes -- VideoFrame::visible_data()
// returns an empty span for kStorageOpaque -- so there is nothing here for a
// CPU graph to read. The stage refuses the frame instead of ToI420()ing it,
// because that would drag every frame back to system memory and quietly undo
// the zero-copy path: the most expensive way to produce a picture that never
// needed to move.
//
// Pinned at the stage because this is the last gate before null plane
// pointers reach avfilter. Before it existed, the only trace was a
// "buffersrc rejected a frame" line, which reads as an FFmpeg fault rather
// than the configuration conflict it is.
TEST(VideoFilterTest, RefusesAHardwareFrame) {
  FFmpegVideoFilter filter;
  ASSERT_TRUE(filter.Initialize("null", VideoFormat::kNV12, kSize));

  static int payload = 0;
  NativeHandle handle;
  handle.kind = NativeHandleKind::kVaapiSurface;
  handle.id = &payload;
  auto hw = VideoFrame::WrapNativeBuffer(
      handle, VideoFormat::kNV12, kSize, kSize, Rational{1, 1},
      base::TimeDelta(), base::Milliseconds(33), 0, base::BindOnce([] {}),
      base::BindRepeating([] { return base::scoped_refptr<VideoFrame>(); }));
  ASSERT_FALSE(hw->IsMappable());

  // Seeded with the input so a stage that silently did nothing and returned
  // early would still leave a non-null |out| behind.
  base::scoped_refptr<VideoFrame> out = hw;
  EXPECT_FALSE(filter.Process(hw, &out));
  EXPECT_FALSE(out);
  // The reason, not just the failure. Letting the frame reach avfilter would
  // also return false -- it just gets there by handing over null plane
  // pointers, which is undefined behaviour whose only visible symptom used to
  // be a log line blaming FFmpeg. Asserting on the reason is what separates
  // "refused on purpose" from "rejected downstream".
  EXPECT_EQ(VideoFilterState::kRefusedHardwareFrame, filter.last_rejection());
}

// A successful call clears the reason, so a stale refusal can never be read
// back as the explanation for a frame that went through fine.
//
// The refusal has to come FIRST, and on the same stage. A stage that had never
// refused starts with no reason either, so a test that only ever fed it good
// frames would pass just as well on a stage that never cleared one -- it would
// be asserting on the constructor.
TEST(VideoFilterTest, SuccessClearsTheRejectionReason) {
  FFmpegVideoFilter filter;
  ASSERT_TRUE(filter.Initialize("null", VideoFormat::kI420, kSize));

  // Refused on geometry rather than on storage, so the reason is recorded by
  // the same code path a hardware frame takes but without needing a native
  // handle: same format, different coded size.
  auto wrong = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{32, 24}, Size{32, 24}, Rational{1, 1},
      base::TimeDelta(), base::Milliseconds(33), 0);
  base::scoped_refptr<VideoFrame> ignored;
  ASSERT_FALSE(filter.Process(wrong, &ignored));
  ASSERT_TRUE(filter.last_rejection().has_value());

  auto in = MakeGradientFrame();
  base::scoped_refptr<VideoFrame> out;
  ASSERT_TRUE(filter.Process(in, &out));
  EXPECT_FALSE(filter.last_rejection().has_value());
}

// The other half of the same decision: a frame the graph CAN read is handed
// to it, and the graph's answer is what gets shown. Without this the refusal
// test above would pass just as well on a stage that refused everything.
TEST(VideoFilterTest, AcceptsAMappableFrame) {
  FFmpegVideoFilter filter;
  ASSERT_TRUE(filter.Initialize("hflip", VideoFormat::kI420, kSize));

  auto in = MakeGradientFrame();
  ASSERT_TRUE(in->IsMappable());
  base::scoped_refptr<VideoFrame> out;
  ASSERT_TRUE(filter.Process(in, &out));
  ASSERT_TRUE(out);
  const auto* src = in->visible_data(VideoFrame::kYPlane).data();
  const auto* dst = out->visible_data(VideoFrame::kYPlane).data();
  EXPECT_EQ(dst[0], src[63]);
  EXPECT_EQ(dst[63], src[0]);
}

}  // namespace
}  // namespace avbase::media
