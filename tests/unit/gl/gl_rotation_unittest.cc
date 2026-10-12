// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.
//
// That a rotated frame is DRAWN rotated, checked by reading pixels back.
//
// Why this file has to exist at all: the display rotation was parsed out of the
// container, carried on the stream config, reported to the host as
// `Player::video_rotation()` -- and then dropped. `VideoFrame::rotation()` had
// no writer anywhere in the tree, so it read back 0 for every frame ever
// produced, and a renderer that honoured it would have honoured a constant.
// Parsed, reported, and then lost on the way to the pixels.
//
// So the tests below cover the whole distance, not just the last hop: the size
// a layout has to plan for (§1), the angle a layer ends up drawn at (§2), and
// the picture that reaches the framebuffer (§3).

#include <memory>
#include <optional>
#include <string>

#include "gl_test_context.h"
#include "gl_test_frames.h"
#include "gtest/gtest.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"

namespace avbase::media {
namespace {

using ::avbase::test::Describe;
using ::avbase::test::GlTestWindow;
using ::avbase::test::kBlackLuma;
using ::avbase::test::kBrightThreshold;
using ::avbase::test::kDarkThreshold;
using ::avbase::test::kNeutralChroma;
using ::avbase::test::kSampleInset;
using ::avbase::test::kWhiteLuma;
using ::avbase::test::MakeTopHalfFrameRotated;
using ::avbase::test::ReadPixel;
using ::avbase::test::Rgb;
using gl::GlApi;

constexpr int kCanvasSize = 64;

// Where the white half of the frame has to end up, per quarter turn. The frame
// is stored white-on-top, so turning it clockwise by |rotation| carries that
// white half to a different edge each time; a renderer that ignores the
// rotation leaves it at the top for all four.
enum class WhiteEdge { kTop, kRight, kBottom, kLeft };

WhiteEdge ExpectedWhiteEdge(int rotation) {
  switch (rotation) {
  case 0:
    return WhiteEdge::kTop;
  case 90:
    return WhiteEdge::kRight;
  case 180:
    return WhiteEdge::kBottom;
  default:
    return WhiteEdge::kLeft;
  }
}

class RotationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string error;
    window_ = GlTestWindow::Create(kCanvasSize, kCanvasSize, &error);
    if (!window_) {
      GTEST_SKIP() << error;
    }
    presenter_ = std::make_unique<GlPresenter>(window_->TakeContext());
    ASSERT_TRUE(presenter_->Init());
  }

  // Composes a one-layer scene carrying |frame| into the canvas, blits it, and
  // returns the red channel sampled at the middle of each of the four edges.
  // Stops before the swap so the back buffer is still defined -- see
  // gl_present.h.
  struct Edges {
    int top{0};
    int bottom{0};
    int left{0};
    int right{0};
  };
  Edges DrawAndSample(int rotation, std::optional<int> override_degrees) {
    std::string error;
    auto scene = GlScene::Create(*presenter_->api(), &error);
    EXPECT_TRUE(scene) << error;
    if (!scene) {
      return {};
    }
    const GlScene::LayerId layer = scene->AddVideoLayer(0);
    EXPECT_NE(layer, GlScene::kNoLayer);
    if (layer == GlScene::kNoLayer) {
      return {};
    }
    base::scoped_refptr<VideoFrame> frame = MakeTopHalfFrameRotated(
        VideoFormat::kI420, window_->width(), window_->height(), kWhiteLuma,
        kBlackLuma, rotation);
    EXPECT_TRUE(frame);
    if (!frame) {
      return {};
    }
    EXPECT_TRUE(scene->Upload(layer, *frame));
    if (override_degrees) {
      EXPECT_TRUE(scene->SetLayerRotation(layer, override_degrees));
    }
    EXPECT_TRUE(presenter_->ComposeScene(*scene));
    presenter_->PresentComposed();

    const gl::GlApi* api = presenter_->api();
    const int w = window_->width();
    const int h = window_->height();
    return Edges{
        ReadPixel(*api, w / 2, h - 1 - kSampleInset, GL_BACK).r,
        ReadPixel(*api, w / 2, kSampleInset, GL_BACK).r,
        ReadPixel(*api, kSampleInset, h / 2, GL_BACK).r,
        ReadPixel(*api, w - 1 - kSampleInset, h / 2, GL_BACK).r,
    };
  }

  std::unique_ptr<GlTestWindow> window_;
  std::unique_ptr<GlPresenter> presenter_;
};

// ---- §1 The size a layout has to plan for ----------------------------------

TEST_F(RotationTest, AQuarterTurnSwapsTheAxesAndAHalfTurnDoesNot) {
  const Size natural{640, 480};
  for (const int rotation : {0, 90, 180, 270}) {
    auto frame = VideoFrame::CreateBlackFrame(
        VideoFormat::kI420, natural, natural, Rational{1, 1}, base::TimeDelta(),
        base::TimeDelta(), 0);
    ASSERT_TRUE(frame);
    frame->set_rotation(rotation);

    const bool transposed = rotation == 90 || rotation == 270;
    const Size expected = transposed ? Size{480, 640} : natural;
    EXPECT_EQ(frame->NaturalSizeForRotation(), expected)
        << "rotation=" << rotation;
    // The undecorated size must stay the decoder's: code that wants the coded
    // geometry (allocations, stride maths) has to still be able to get it.
    EXPECT_EQ(frame->natural_size(), natural);
  }
}

// The snap: an angle no stage can carry still has to land on one it can.
TEST_F(RotationTest, AnAngleThatIsNotAQuarterTurnSnapsToTheNearestOne) {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{640, 480}, Size{640, 480}, Rational{1, 1},
      base::TimeDelta(), base::TimeDelta(), 0);
  ASSERT_TRUE(frame);
  // Values deliberately NOT on the boundary: 315 is 45 from both 270 and 0,
  // so it cannot say which way the snap leans.
  for (const auto& [input, expected] :
       {std::pair<int, int>{89, 90}, {44, 0}, {300, 270}, {350, 0}}) {
    frame->set_rotation(input);
    EXPECT_EQ(SnapLayerRotation(frame->rotation()), expected)
        << "input=" << input;
  }
  // A negative angle is a counter-clockwise turn; it has to fold back onto
  // the same four answers rather than producing a negative index.
  frame->set_rotation(-90);
  EXPECT_EQ(SnapLayerRotation(frame->rotation()), 270);
}

// --- §2 Which way a layer is drawn -----------------------------------------

// The default: a layer shows its frame the way the frame says. This is the one
// that was broken -- with the frame's rotation never written, "follow the
// frame" and "ignore the frame" were the same code.
TEST_F(RotationTest, ALayerFollowsTheRotationOfTheFrameItWasGiven) {
  std::string error;
  auto scene = GlScene::Create(*presenter_->api(), &error);
  ASSERT_TRUE(scene) << error;
  const auto layer = scene->AddVideoLayer(0);
  ASSERT_NE(layer, GlScene::kNoLayer);

  base::scoped_refptr<VideoFrame> frame = MakeTopHalfFrameRotated(
      VideoFormat::kI420, window_->width(), window_->height(), kWhiteLuma,
      kBlackLuma, /*rotation=*/90);
  ASSERT_TRUE(frame);
  ASSERT_TRUE(scene->Upload(layer, *frame));

  EXPECT_EQ(scene->LayerRotation(layer), 90);
}

// An override is for a host that is turning a layer on purpose; it has to win
// over the frame, and it has to be possible to hand the layer back.
TEST_F(RotationTest, AnOverridePinsTheAngleAndCanBeHandedBack) {
  std::string error;
  auto scene = GlScene::Create(*presenter_->api(), &error);
  ASSERT_TRUE(scene) << error;
  const auto layer = scene->AddVideoLayer(0);
  ASSERT_NE(layer, GlScene::kNoLayer);

  base::scoped_refptr<VideoFrame> frame = MakeTopHalfFrameRotated(
      VideoFormat::kI420, window_->width(), window_->height(), kWhiteLuma,
      kBlackLuma, /*rotation=*/90);
  ASSERT_TRUE(frame);
  ASSERT_TRUE(scene->Upload(layer, *frame));

  ASSERT_TRUE(scene->SetLayerRotation(layer, 0));
  EXPECT_EQ(scene->LayerRotation(layer), 0);

  ASSERT_TRUE(scene->SetLayerRotation(layer, std::nullopt));
  EXPECT_EQ(scene->LayerRotation(layer), 90) << "nullopt resumes following";

  // And it is snapped, so what the host asked for is not what it is told.
  ASSERT_TRUE(scene->SetLayerRotation(layer, 89));
  EXPECT_EQ(scene->LayerRotation(layer), 90);
}

// ---- §3 The picture that reaches the framebuffer --------------------------

// Four turns of ONE frame: the white half has to land on a different edge each
// time. Comparing orientations of the same picture is what makes the assertion
// about the rotation rather than about the colour pipeline -- every sample goes
// through the same matrix, so the matrix cannot be what makes them differ.
TEST_F(RotationTest, TheWhiteHalfReachesADifferentEdgeAtEachQuarterTurn) {
  int bright_edges = 0;
  for (const int rotation : {0, 90, 180, 270}) {
    const Edges e = DrawAndSample(rotation, std::nullopt);
    const WhiteEdge expected = ExpectedWhiteEdge(rotation);
    // The white edge is bright; its opposite is dark; the other two straddle
    // the boundary and are not asserted on.
    switch (expected) {
    case WhiteEdge::kTop:
      EXPECT_GT(e.top, kBrightThreshold) << rotation;
      EXPECT_LT(e.bottom, kDarkThreshold) << rotation;
      break;
    case WhiteEdge::kRight:
      EXPECT_GT(e.right, kBrightThreshold) << rotation;
      EXPECT_LT(e.left, kDarkThreshold) << rotation;
      break;
    case WhiteEdge::kBottom:
      EXPECT_GT(e.bottom, kBrightThreshold) << rotation;
      EXPECT_LT(e.top, kDarkThreshold) << rotation;
      break;
    case WhiteEdge::kLeft:
      EXPECT_GT(e.left, kBrightThreshold) << rotation;
      EXPECT_LT(e.right, kDarkThreshold) << rotation;
      break;
    }
    ++bright_edges;
  }
  EXPECT_EQ(bright_edges, 4);
}

// The single-frame path is the one a player runs, and the one the rotation has
// to be honoured on for a phone-shot clip to come out upright.
TEST_F(RotationTest, TheSingleFramePathHonoursTheFramesRotation) {
  base::scoped_refptr<VideoFrame> frame = MakeTopHalfFrameRotated(
      VideoFormat::kI420, window_->width(), window_->height(), kWhiteLuma,
      kBlackLuma, /*rotation=*/270);
  ASSERT_TRUE(frame);
  ASSERT_TRUE(presenter_->Compose(*frame));
  presenter_->PresentComposed();

  const gl::GlApi* api = presenter_->api();
  const int w = window_->width();
  const int h = window_->height();
  // 270 clockwise = 90 counter-clockwise: the top of the frame turns to the
  // LEFT, so the white half is on the left and the black half on the right.
  EXPECT_GT(ReadPixel(*api, kSampleInset, h / 2, GL_BACK).r, kBrightThreshold);
  EXPECT_LT(ReadPixel(*api, w - 1 - kSampleInset, h / 2, GL_BACK).r,
            kDarkThreshold);
}

}  // namespace
}  // namespace avbase::media
