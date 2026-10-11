// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// How the three planar layouts are carved into textures, checked by reading the
// colour back.
//
// Two failure modes live here and neither one raises a GL error:
//
//   * The rows of a plane are further apart than the image is wide. Every frame
//     this tree allocates aligns its rows to 64 bytes (VideoFrame::
//     CreateBlackFrame, which the software decoder uses), so a 320x240 video
//     has a 192-byte chroma row where only 160 bytes are image. An upload that
//     ignores the pitch reads the plane as if packed and lands a sheared copy
//     in the texture: luma survives and the chroma is wrong. The texture is
//     not black, not garbage, just incorrect -- which is why it took a
//     read-back test to find.
//
//   * YV12 stores V before U and NV12 interleaves them, while the shader has
//     fixed units for U and V. Getting the mapping wrong swaps red and blue.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "gl_test_context.h"
#include "gl_test_frames.h"
#include "gtest/gtest.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_present.h"

namespace avbase::media {
namespace {

using ::avbase::test::Describe;
using ::avbase::test::Fill;
using ::avbase::test::Rgb;

constexpr int kSize = 64;

// Limited-range BT.601 luma for mid grey, and chroma far enough apart that
// swapping U and V moves red against blue rather than nudging a hue.
constexpr uint8_t kMidLuma = 128;
constexpr uint8_t kLowChroma = 64;
constexpr uint8_t kHighChroma = 192;

class PlaneUploadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string error;
    window_ = test::GlTestWindow::Create(kSize, kSize, &error);
    if (!window_) {
      GTEST_SKIP() << error;
    }
    presenter_ = std::make_unique<GlPresenter>(window_->TakeContext());
    ASSERT_TRUE(presenter_->Init());
  }

  // Renders one uniform frame and returns a column of pixels sampled from the
  // middle of the presented image, bottom row first (glReadPixels order).
  std::vector<Rgb> RenderColumn(VideoFormat format, uint8_t y, uint8_t u,
                                uint8_t v) {
    Size size{kSize, kSize};
    base::scoped_refptr<VideoFrame> frame =
        VideoFrame::CreateBlackFrame(format, size, size, Rational{1, 1},
                                     base::TimeDelta(), base::TimeDelta(), 0);
    if (!frame || !Fill(frame, y, u, v)) {
      return {};
    }
    // Compose + PresentComposed, deliberately NOT Present(): the swap is what
    // hands the pixels to the window, and the back buffer's contents after it
    // are undefined -- reading them gives black or the previous frame, which
    // looks like a rendering bug and is not one.
    if (!presenter_->Compose(*frame)) {
      return {};
    }
    presenter_->PresentComposed();
    const gl::GlApi* api = presenter_->api();
    const int width = window_->width();
    const int height = window_->height();
    std::vector<uint8_t> row(static_cast<size_t>(width) * 4);
    api->context().ReadBuffer(GL_BACK);
    api->context().PixelStorei(GL_PACK_ALIGNMENT, 1);
    std::vector<Rgb> column;
    for (int y_pos = 4; y_pos < height - 4; y_pos += 4) {
      api->context().ReadPixels(width / 2, y_pos, 1, 1, GL_RGBA,
                                GL_UNSIGNED_BYTE, row.data());
      column.push_back(Rgb{row[0], row[1], row[2]});
    }
    return column;
  }

  std::unique_ptr<test::GlTestWindow> window_;
  std::unique_ptr<GlPresenter> presenter_;
};

// The regression test for the pitch bug: a uniform grey frame must come out
// uniform and grey. With the row length ignored, the chroma texture mixes the
// plane's visible 128s with the zero bytes of its row padding, and the image
// comes out banded and green-tinted instead.
TEST_F(PlaneUploadTest, AUniformGreyFrameStaysUniformAndGrey) {
  const std::vector<Rgb> column =
      RenderColumn(VideoFormat::kI420, kMidLuma, 128, 128);
  ASSERT_FALSE(column.empty());

  for (const Rgb& c : column) {
    // Neutral: a chroma that is not (128, 128) tints the grey, and the tint is
    // what the sheared rows produce.
    EXPECT_LE(std::abs(c.r - c.g), 3) << "not neutral at " << Describe(c);
    EXPECT_LE(std::abs(c.b - c.g), 3) << "not neutral at " << Describe(c);
    // And mid grey, rather than the black or white ends of the range.
    EXPECT_GT(c.g, 120) << "too dark at " << Describe(c);
    EXPECT_LT(c.g, 145) << "too bright at " << Describe(c);
  }
}

// YV12 is not a synonym for I420: its second plane holds V. The same logical
// (U, V) must therefore render identically through both, which it does not if
// the plane swap is dropped.
TEST_F(PlaneUploadTest, Yv12RendersLikeI420ForTheSameLogicalChroma) {
  const std::vector<Rgb> i420 =
      RenderColumn(VideoFormat::kI420, kMidLuma, kLowChroma, kHighChroma);
  const std::vector<Rgb> yv12 =
      RenderColumn(VideoFormat::kYV12, kMidLuma, kLowChroma, kHighChroma);
  ASSERT_EQ(i420.size(), yv12.size());
  ASSERT_FALSE(i420.empty());
  for (size_t i = 0; i < i420.size(); ++i) {
    EXPECT_TRUE(i420[i] == yv12[i])
        << "row " << i << ": I420 " << Describe(i420[i]) << " vs YV12 "
        << Describe(yv12[i]);
  }
}

// NV12's chroma is one plane of interleaved pairs, so its upload row length is
// measured in PAIRS, not bytes. Passing the byte stride instead reads twice as
// far per row and shears the same way the I420 padding did.
TEST_F(PlaneUploadTest, Nv12RendersLikeI420ForTheSameLogicalChroma) {
  const std::vector<Rgb> i420 =
      RenderColumn(VideoFormat::kI420, kMidLuma, kLowChroma, kHighChroma);
  const std::vector<Rgb> nv12 =
      RenderColumn(VideoFormat::kNV12, kMidLuma, kLowChroma, kHighChroma);
  ASSERT_EQ(i420.size(), nv12.size());
  ASSERT_FALSE(i420.empty());
  for (size_t i = 0; i < i420.size(); ++i) {
    EXPECT_TRUE(i420[i] == nv12[i])
        << "row " << i << ": I420 " << Describe(i420[i]) << " vs NV12 "
        << Describe(nv12[i]);
  }
}

}  // namespace
}  // namespace avbase::media
