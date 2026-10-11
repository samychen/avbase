// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// What the picture looks like, checked by reading it back.
//
// The two-stage GL path has exactly one failure mode that no return code can
// catch: the composite lands in the canvas the right way up, and then the blit
// puts it on the window the wrong way up. Every call succeeds, every GL error
// is GL_NO_ERROR, and the only symptom is an upside-down video.
//
// So these tests draw a frame whose top and bottom cannot be confused (top
// half white, bottom half black in limited-range luma) and assert on the
// pixels that come out of the back buffer. Flipping GlBlitter's quad makes
// them fail -- which is the property being protected, and was verified by
// doing exactly that.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "gl_test_context.h"
#include "gtest/gtest.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_present.h"

namespace avbase::media {
namespace {

constexpr int kWindowSize = 64;

// Limited-range BT.601: luma 235 is white, 16 is black, and the chroma planes
// sit at 128 so the luma decision is the only thing the colour matrix can get
// wrong.
constexpr uint8_t kWhiteLuma = 235;
constexpr uint8_t kBlackLuma = 16;
constexpr uint8_t kNeutralChroma = 128;

// A red-flag threshold rather than a tolerance: this is 0 vs 235 luma through a
// known matrix, so anything in between means a different bug than orientation.
constexpr int kBrightThreshold = 200;
constexpr int kDarkThreshold = 50;

// Crops the window to a multiple of four and leaves a margin at both ends, so
// a bilinear sample that reaches one row past a half-way edge cannot decide the
// answer.
constexpr int kSampleInset = 4;

// An owned I420 frame with white over black, split horizontally at the middle:
// row 0 is white, the last row is black.
base::scoped_refptr<VideoFrame> MakeTopWhiteFrame(int width, int height) {
  Size size{width, height};
  base::scoped_refptr<VideoFrame> frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, size, size, Rational{1, 1}, base::TimeDelta(),
      base::TimeDelta(), 0);
  if (!frame) {
    return nullptr;
  }
  const std::span<uint8_t> luma = frame->mutable_data(VideoFrame::kYPlane);
  const int32_t stride = frame->stride(VideoFrame::kYPlane);
  for (int y = 0; y < height; ++y) {
    const uint8_t value = y < height / 2 ? kWhiteLuma : kBlackLuma;
    std::fill_n(luma.begin() + static_cast<size_t>(y) * stride, width, value);
  }
  for (const VideoFrame::Plane plane :
       {VideoFrame::kUPlane, VideoFrame::kVPlane}) {
    const std::span<uint8_t> chroma = frame->mutable_data(plane);
    const int32_t chroma_stride = frame->stride(plane);
    for (int y = 0; y < height / 2; ++y) {
      std::fill_n(chroma.begin() + static_cast<size_t>(y) * chroma_stride,
                  width / 2, kNeutralChroma);
    }
  }
  return frame;
}

// Reads one row of the default framebuffer's back buffer -- what the blit just
// produced, before it is handed to the window. glReadPixels counts y from the
// BOTTOM, so y = 0 is the bottom of the picture.
std::vector<uint8_t> ReadBackRow(const gl::GlApi& api, int width, int y) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * 4);
  api.context().ReadBuffer(GL_BACK);
  api.context().PixelStorei(GL_PACK_ALIGNMENT, 1);
  api.context().ReadPixels(0, y, width, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                           pixels.data());
  return pixels;
}

int RedOf(const std::vector<uint8_t>& row) {
  return row.empty() ? -1 : row[0];
}

// Draws one frame all the way to the framebuffer, without the swap, so the
// result can be read back.
class OrientationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string error;
    window_ = test::GlTestWindow::Create(kWindowSize, kWindowSize, &error);
    if (!window_) {
      GTEST_SKIP() << error;
    }
    presenter_ = std::make_unique<GlPresenter>(window_->TakeContext());
    ASSERT_TRUE(presenter_->Init());
  }

  // Composes and blits |frame|, optionally into a canvas of its own size.
  void Draw(int canvas_width = 0, int canvas_height = 0) {
    base::scoped_refptr<VideoFrame> frame =
        MakeTopWhiteFrame(kWindowSize, kWindowSize);
    ASSERT_TRUE(frame);
    presenter_->SetCanvasSize(canvas_width, canvas_height);
    ASSERT_TRUE(presenter_->Compose(*frame));
    presenter_->PresentComposed();
  }

  std::unique_ptr<test::GlTestWindow> window_;
  std::unique_ptr<GlPresenter> presenter_;
};

// The load-bearing one. If the blitter's quad is not flipped relative to the
// render quad, the two assertions below swap places.
TEST_F(OrientationTest, TheTopOfTheFrameReachesTheTopOfTheWindow) {
  Draw();

  const gl::GlApi* api = presenter_->api();
  ASSERT_TRUE(api);
  const int width = window_->width();
  const int height = window_->height();
  ASSERT_GE(height, 2 * kSampleInset + 2);

  const int top = RedOf(ReadBackRow(*api, width, height - 1 - kSampleInset));
  const int bottom = RedOf(ReadBackRow(*api, width, kSampleInset));

  EXPECT_GT(top, kBrightThreshold)
      << "the top of the window should show the white half of the frame";
  EXPECT_LT(bottom, kDarkThreshold)
      << "the bottom of the window should show the black half of the frame";
}

// The whole point of the canvas: composing at one size and displaying at
// another. A 2x-oversampled canvas would show the same picture; the test uses
// half-size, which is the direction that also exercises the upscale filter.
TEST_F(OrientationTest, ACanvasOfADifferentSizeStillLandsUpright) {
  Draw(/*canvas_width=*/kWindowSize / 2, /*canvas_height=*/kWindowSize / 2);

  const gl::GlApi* api = presenter_->api();
  ASSERT_TRUE(api);
  const int width = window_->width();
  const int height = window_->height();

  EXPECT_GT(RedOf(ReadBackRow(*api, width, height - 1 - kSampleInset)),
            kBrightThreshold)
      << "scaling through the canvas must not flip the picture";
  EXPECT_LT(RedOf(ReadBackRow(*api, width, kSampleInset)), kDarkThreshold);
}

// Present() is the sink's entry point, and the only one that swaps. Every
// other test here stops at PresentComposed() to keep the pixels readable, so
// this one exists to prove the composed path is what Present() actually runs.
TEST_F(OrientationTest, PresentRunsTheSameComposedPath) {
  base::scoped_refptr<VideoFrame> frame =
      MakeTopWhiteFrame(kWindowSize, kWindowSize);
  ASSERT_TRUE(frame);

  PresentTiming timing;
  ASSERT_TRUE(presenter_->Present(*frame, &timing));
  // Not a timing assertion -- it is an assertion that the stages RAN. A blit
  // that silently did nothing would leave blit_ms at exactly 0.0 and still
  // return true.
  EXPECT_GT(timing.blit_ms, 0.0);
  EXPECT_GE(timing.upload_ms, 0.0);
}

}  // namespace
}  // namespace avbase::media
