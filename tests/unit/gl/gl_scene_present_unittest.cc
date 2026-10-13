// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The multi-source path, checked the same way the single-frame path is: by
// reading the pixels that reach the window.
//
// A scene adds exactly one new way to be wrong that no return code catches --
// layers composed into off-screen targets and mixed back out can be upside
// down, or in the wrong order, or blended at the wrong weight -- and all three
// are invisible to every call's result. So these tests compose two scenes of
// known colour, cross-fade them, and assert on the window's back buffer.
//
// They also cover the two properties the single-frame tests cannot: that the
// scene path reaches the same window through the same blit, and that a layer
// composed by a scene and then sampled back out survives the round trip
// upright. The second one is where the transition's flip lives -- the first
// version of GlTransition had it the wrong way round and this test was the
// only thing that noticed.

#include <limits>
#include <memory>
#include <string>

#include "gl_test_context.h"
#include "gl_test_frames.h"
#include "gtest/gtest.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"
#include "render/gl/gl_api.h"

namespace avbase::media {
namespace {

constexpr int kWindowSize = 64;

// A cross-fade at half weight should land near the middle of those two, which
// is 127.5 -- so a band around it, wide enough for the 8-bit round trip and
// narrow enough that "showed one side only" misses it.
constexpr int kMidLow = 100;
constexpr int kMidHigh = 156;

class ScenePresentTest : public ::testing::Test {
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

  std::unique_ptr<GlScene> NewScene() {
    std::string error;
    std::unique_ptr<GlScene> scene =
        GlScene::Create(*presenter_->api(), &error);
    EXPECT_TRUE(scene) << error;
    return scene;
  }

  // Red channel of one pixel of the window's back buffer. Every frame used
  // here is neutral in chroma, so all three channels carry the same value and
  // naming one of them is enough. y is measured from the bottom, as
  // glReadPixels does.
  int WindowRed(int y) {
    return test::ReadPixel(*presenter_->api(), window_->width() / 2, y, GL_BACK)
        .r;
  }
  int WindowTopRed() {
    return WindowRed(window_->height() - 1 - test::kSampleInset);
  }
  int WindowBottomRed() { return WindowRed(test::kSampleInset); }

  // Red channel of one pixel of the window's back buffer, addressed the way a
  // wipe's DIRECTION is described: x from the left, y from the TOP.
  int WindowRedAt(int x, int y_from_top) {
    return test::ReadPixel(*presenter_->api(), x,
                           window_->height() - 1 - y_from_top, GL_BACK)
        .r;
  }

  // A one-layer scene showing one flat colour, so "which side of a
  // transition's edge is this pixel on" has a yes/no answer everywhere. Both
  // frames are neutral in chroma, so the red channel alone carries it.
  std::unique_ptr<GlScene> FlatScene(uint8_t luma) {
    std::unique_ptr<GlScene> scene = NewScene();
    if (!scene) {
      return scene;
    }
    const GlScene::LayerId layer = scene->AddVideoLayer(0);
    EXPECT_NE(layer, GlScene::kNoLayer);
    base::scoped_refptr<VideoFrame> frame =
        test::MakeSolidFrame(VideoFormat::kI420, kWindowSize, kWindowSize, luma,
                             test::kNeutralChroma, test::kNeutralChroma);
    EXPECT_TRUE(frame);
    EXPECT_TRUE(presenter_->UploadLayer(*scene, layer, *frame));
    return scene;
  }

  // Transitions a WHITE scene into a BLACK one at |progress| = 1/4 and returns
  // the red channel at both ends of the axis the wipe travels along: |start|
  // is the end the edge sets out from, |end| the end it is headed for.
  //
  // Sampling both ends is what pins a direction -- at 1/4 progress every wipe
  // covers a quarter of the picture, and only WHICH quarter differs.
  std::pair<int, int> WipeEnds(gl::TransitionKind kind, bool horizontal) {
    std::unique_ptr<GlScene> from = FlatScene(test::kWhiteLuma);
    std::unique_ptr<GlScene> to = FlatScene(test::kBlackLuma);
    EXPECT_TRUE(from);
    EXPECT_TRUE(to);
    EXPECT_TRUE(presenter_->SetTransitionKind(kind));
    EXPECT_TRUE(presenter_->ComposeTransition(*from, *to, /*progress=*/0.25f));
    presenter_->PresentComposed();
    const int x = window_->width();
    const int y = window_->height();
    if (horizontal) {
      return {WindowRedAt(x / 8, y / 2), WindowRedAt(7 * x / 8, y / 2)};
    }
    return {WindowRedAt(x / 2, y / 8), WindowRedAt(x / 2, 7 * y / 8)};
  }

  std::unique_ptr<test::GlTestWindow> window_;
  std::unique_ptr<GlPresenter> presenter_;
};

// The scene path has to reach the window through the same compose -> blit pair
// the single-frame path uses, including the blit's flip. A scene that composed
// correctly into the canvas and then went out through an upright quad would
// fail only here.
TEST_F(ScenePresentTest, ASingleLayerSceneLandsUprightOnTheWindow) {
  std::unique_ptr<GlScene> scene = NewScene();
  ASSERT_TRUE(scene);
  const GlScene::LayerId layer = scene->AddVideoLayer(0);
  ASSERT_NE(layer, GlScene::kNoLayer);

  base::scoped_refptr<VideoFrame> frame =
      test::MakeTopHalfFrame(VideoFormat::kI420, kWindowSize, kWindowSize,
                             test::kWhiteLuma, test::kBlackLuma);
  ASSERT_TRUE(frame);
  ASSERT_TRUE(presenter_->UploadLayer(*scene, layer, *frame));
  ASSERT_TRUE(presenter_->ComposeScene(*scene));
  presenter_->PresentComposed();

  EXPECT_GT(WindowTopRed(), test::kBrightThreshold)
      << "the top of the window should show the white half of the layer";
  EXPECT_LT(WindowBottomRed(), test::kDarkThreshold)
      << "the bottom of the window should show the black half of the layer";
}

// Two layers covering the same rect: which one wins is exactly the z order.
// The second half of the test moves the loser on top, so an implementation
// that drew in insertion order instead of z order fails it.
TEST_F(ScenePresentTest, TheHigherLayerWinsAndZCanBeChanged) {
  std::unique_ptr<GlScene> scene = NewScene();
  ASSERT_TRUE(scene);

  const GlScene::LayerId white = scene->AddVideoLayer(/*z=*/0);
  const GlScene::LayerId black = scene->AddVideoLayer(/*z=*/5);
  ASSERT_NE(white, GlScene::kNoLayer);
  ASSERT_NE(black, GlScene::kNoLayer);

  base::scoped_refptr<VideoFrame> bright = test::MakeSolidFrame(
      VideoFormat::kI420, kWindowSize, kWindowSize, test::kWhiteLuma,
      test::kNeutralChroma, test::kNeutralChroma);
  base::scoped_refptr<VideoFrame> dark = test::MakeSolidFrame(
      VideoFormat::kI420, kWindowSize, kWindowSize, test::kBlackLuma,
      test::kNeutralChroma, test::kNeutralChroma);
  ASSERT_TRUE(bright);
  ASSERT_TRUE(dark);
  ASSERT_TRUE(presenter_->UploadLayer(*scene, white, *bright));
  ASSERT_TRUE(presenter_->UploadLayer(*scene, black, *dark));

  ASSERT_TRUE(presenter_->ComposeScene(*scene));
  presenter_->PresentComposed();
  EXPECT_LT(WindowTopRed(), test::kDarkThreshold)
      << "z=5 is on top of z=0, so the window should be black";

  // Same layers, same frames, one reorder -- and nothing re-uploaded, which is
  // the point of z being a field rather than a draw-order list.
  ASSERT_TRUE(scene->SetLayerZ(black, /*z=*/-5));
  ASSERT_TRUE(presenter_->ComposeScene(*scene));
  presenter_->PresentComposed();
  EXPECT_GT(WindowTopRed(), test::kBrightThreshold)
      << "moving the white layer above the others should show it";
}

// A layer composed into a framebuffer, then sampled back out by the transition
// and composed into the canvas, has passed through two off-screen targets. Its
// top must still be at the top: with the transition's quad unflipped this
// reads the black half at the top, which is how the mistake was found.
TEST_F(ScenePresentTest, ATransitionAtZeroIsTheFromSceneUpright) {
  std::unique_ptr<GlScene> from = NewScene();
  std::unique_ptr<GlScene> to = NewScene();
  ASSERT_TRUE(from);
  ASSERT_TRUE(to);
  const GlScene::LayerId from_layer = from->AddVideoLayer(0);
  const GlScene::LayerId to_layer = to->AddVideoLayer(0);
  ASSERT_NE(from_layer, GlScene::kNoLayer);
  ASSERT_NE(to_layer, GlScene::kNoLayer);

  base::scoped_refptr<VideoFrame> split =
      test::MakeTopHalfFrame(VideoFormat::kI420, kWindowSize, kWindowSize,
                             test::kWhiteLuma, test::kBlackLuma);
  base::scoped_refptr<VideoFrame> dark = test::MakeSolidFrame(
      VideoFormat::kI420, kWindowSize, kWindowSize, test::kBlackLuma,
      test::kNeutralChroma, test::kNeutralChroma);
  ASSERT_TRUE(split);
  ASSERT_TRUE(dark);
  ASSERT_TRUE(presenter_->UploadLayer(*from, from_layer, *split));
  ASSERT_TRUE(presenter_->UploadLayer(*to, to_layer, *dark));

  ASSERT_TRUE(presenter_->ComposeTransition(*from, *to, /*progress=*/0.0f));
  presenter_->PresentComposed();

  EXPECT_GT(WindowTopRed(), test::kBrightThreshold)
      << "progress 0 shows the from scene, and it must be upright";
  EXPECT_LT(WindowBottomRed(), test::kDarkThreshold);

  ASSERT_TRUE(presenter_->ComposeTransition(*from, *to, /*progress=*/1.0f));
  presenter_->PresentComposed();
  EXPECT_LT(WindowTopRed(), test::kDarkThreshold)
      << "progress 1 shows the to scene";
}

// Half weight has to be a real blend: a mixer that rounded progress to one end
// would pass both tests above and fail this one.
TEST_F(ScenePresentTest, HalfwayIsAnEvenBlendOfBothScenes) {
  std::unique_ptr<GlScene> from = NewScene();
  std::unique_ptr<GlScene> to = NewScene();
  ASSERT_TRUE(from);
  ASSERT_TRUE(to);
  const GlScene::LayerId from_layer = from->AddVideoLayer(0);
  const GlScene::LayerId to_layer = to->AddVideoLayer(0);
  ASSERT_TRUE(from_layer != GlScene::kNoLayer);
  ASSERT_TRUE(to_layer != GlScene::kNoLayer);

  base::scoped_refptr<VideoFrame> bright = test::MakeSolidFrame(
      VideoFormat::kI420, kWindowSize, kWindowSize, test::kWhiteLuma,
      test::kNeutralChroma, test::kNeutralChroma);
  base::scoped_refptr<VideoFrame> dark = test::MakeSolidFrame(
      VideoFormat::kI420, kWindowSize, kWindowSize, test::kBlackLuma,
      test::kNeutralChroma, test::kNeutralChroma);
  ASSERT_TRUE(bright);
  ASSERT_TRUE(dark);
  ASSERT_TRUE(presenter_->UploadLayer(*from, from_layer, *bright));
  ASSERT_TRUE(presenter_->UploadLayer(*to, to_layer, *dark));

  ASSERT_TRUE(presenter_->ComposeTransition(*from, *to, /*progress=*/0.5f));
  presenter_->PresentComposed();

  EXPECT_GT(WindowTopRed(), kMidLow);
  EXPECT_LT(WindowTopRed(), kMidHigh);
}

// The per-frame half of the NaN policy, pinned to pixels: a draw cannot return
// false, so a non-finite progress degrades to 0 -- the from scene, upright --
// rather than to an unwritten target. (The setter half of the same policy is
// in gl_scene_unittest, where a NaN is rejected instead.)
TEST_F(ScenePresentTest, ANonFiniteProgressDegradesToTheFromScene) {
  std::unique_ptr<GlScene> from = NewScene();
  std::unique_ptr<GlScene> to = NewScene();
  ASSERT_TRUE(from);
  ASSERT_TRUE(to);
  const GlScene::LayerId from_layer = from->AddVideoLayer(0);
  const GlScene::LayerId to_layer = to->AddVideoLayer(0);
  ASSERT_TRUE(from_layer != GlScene::kNoLayer);
  ASSERT_TRUE(to_layer != GlScene::kNoLayer);

  base::scoped_refptr<VideoFrame> bright = test::MakeSolidFrame(
      VideoFormat::kI420, kWindowSize, kWindowSize, test::kWhiteLuma,
      test::kNeutralChroma, test::kNeutralChroma);
  base::scoped_refptr<VideoFrame> dark = test::MakeSolidFrame(
      VideoFormat::kI420, kWindowSize, kWindowSize, test::kBlackLuma,
      test::kNeutralChroma, test::kNeutralChroma);
  ASSERT_TRUE(bright);
  ASSERT_TRUE(dark);
  ASSERT_TRUE(presenter_->UploadLayer(*from, from_layer, *bright));
  ASSERT_TRUE(presenter_->UploadLayer(*to, to_layer, *dark));

  const float nan = std::numeric_limits<float>::quiet_NaN();
  ASSERT_TRUE(presenter_->ComposeTransition(*from, *to, nan));
  presenter_->PresentComposed();
  EXPECT_GT(WindowTopRed(), test::kBrightThreshold)
      << "a NaN progress must not leave the transition undrawn";

  // An infinity is the same decision, not a different one: every non-finite
  // value means "no progress", so +inf lands on the from scene rather than
  // being clamped to 1. Ending a transition early is not a thing to guess at
  // -- a progress that ran off the end is more likely a clock that lost its
  // denominator than a caller that finished early, and the safe end of a
  // cross-fade is the one the caller was already looking at.
  ASSERT_TRUE(presenter_->ComposeTransition(
      *from, *to, std::numeric_limits<float>::infinity()));
  presenter_->PresentComposed();
  EXPECT_GT(WindowTopRed(), test::kBrightThreshold);

  ASSERT_TRUE(presenter_->ComposeTransition(
      *from, *to, -std::numeric_limits<float>::infinity()));
  presenter_->PresentComposed();
  EXPECT_GT(WindowTopRed(), test::kBrightThreshold);
}

// ---- The other kinds -------------------------------------------------------
//
// Cross-fade is a weight that is constant in space; a wipe is one that is not.
// Everything below is about telling the two apart at the SAME progress, because
// a test that only checks 0 and 1 cannot see the difference: both kinds show
// exactly one side at the ends.

// The edge is a CUT, not a fade: at half progress a pixel well inside the
// covered half is still fully |to|, and one well outside is still fully |from|.
// A cross-fade would put both near the middle.
TEST_F(ScenePresentTest, AWipeIsAHardEdgeNotABlend) {
  ASSERT_TRUE(presenter_->SetTransitionKind(gl::TransitionKind::kWipeRight));
  std::unique_ptr<GlScene> from = FlatScene(test::kWhiteLuma);
  std::unique_ptr<GlScene> to = FlatScene(test::kBlackLuma);
  ASSERT_TRUE(from);
  ASSERT_TRUE(to);

  ASSERT_TRUE(presenter_->ComposeTransition(*from, *to, /*progress=*/0.5f));
  presenter_->PresentComposed();

  const int mid = window_->height() / 2;
  EXPECT_LT(WindowRedAt(window_->width() / 8, mid), test::kDarkThreshold)
      << "left of the edge is the to scene, unblended";
  EXPECT_GT(WindowRedAt(7 * window_->width() / 8, mid), test::kBrightThreshold)
      << "right of the edge is still the from scene, unblended";
}

// The two horizontal directions must land on opposite sides. Each covers a
// quarter at 1/4 progress, so only the SIDE tells them apart -- which is what
// makes this a test of the direction and not merely of the wipe.
TEST_F(ScenePresentTest, AWipeRightCoversFromTheLeft) {
  const auto [start, end] = WipeEnds(gl::TransitionKind::kWipeRight, true);
  EXPECT_LT(start, test::kDarkThreshold)
      << "kWipeRight travels rightwards, so to enters from the left";
  EXPECT_GT(end, test::kBrightThreshold)
      << "the edge has not reached the right yet";
}

TEST_F(ScenePresentTest, AWipeLeftCoversFromTheRight) {
  const auto [start, end] = WipeEnds(gl::TransitionKind::kWipeLeft, true);
  EXPECT_LT(end, test::kDarkThreshold)
      << "kWipeLeft travels leftwards, so to enters from the right";
  EXPECT_GT(start, test::kBrightThreshold)
      << "the edge has not reached the left yet";
}

// Vertical, and the reason y is addressed from the TOP here: "down" on screen
// is the direction a viewer would name, and the shader's vTexCoord counts y
// from the bottom because this pass samples framebuffer textures through the
// flipped quad. The enum takes screen words and the vector below carries the
// sign, so a mix-up would show up as two tests failing, not none.
TEST_F(ScenePresentTest, AWipeDownCoversFromTheTop) {
  const auto [start, end] = WipeEnds(gl::TransitionKind::kWipeDown, false);
  EXPECT_LT(start, test::kDarkThreshold)
      << "kWipeDown travels downwards, so to enters from the top";
  EXPECT_GT(end, test::kBrightThreshold)
      << "the edge has not reached the bottom yet";
}

TEST_F(ScenePresentTest, AWipeUpCoversFromTheBottom) {
  const auto [start, end] = WipeEnds(gl::TransitionKind::kWipeUp, false);
  EXPECT_LT(end, test::kDarkThreshold)
      << "kWipeUp travels upwards, so to enters from the bottom";
  EXPECT_GT(start, test::kBrightThreshold)
      << "the edge has not reached the top yet";
}

// The kind has to be able to go BACK, and that is a different property from
// setting it: the program is compiled once and cached, so a switch that only
// ever moved forwards would keep the wipe program while reporting a cross-fade.
// Same progress, two kinds, opposite answers.
TEST_F(ScenePresentTest, TheKindCanBeHandedBackToACrossFade) {
  std::unique_ptr<GlScene> from = FlatScene(test::kWhiteLuma);
  std::unique_ptr<GlScene> to = FlatScene(test::kBlackLuma);
  ASSERT_TRUE(from);
  ASSERT_TRUE(to);

  ASSERT_TRUE(presenter_->SetTransitionKind(gl::TransitionKind::kWipeRight));
  ASSERT_TRUE(presenter_->ComposeTransition(*from, *to, /*progress=*/0.5f));
  presenter_->PresentComposed();
  const int mid = window_->height() / 2;
  ASSERT_LT(WindowRedAt(window_->width() / 8, mid), test::kDarkThreshold)
      << "a wipe at half progress is unblended on the covered side";

  ASSERT_TRUE(presenter_->SetTransitionKind(gl::TransitionKind::kCrossFade));
  ASSERT_EQ(gl::TransitionKind::kCrossFade, presenter_->transition_kind());
  ASSERT_TRUE(presenter_->ComposeTransition(*from, *to, /*progress=*/0.5f));
  presenter_->PresentComposed();
  EXPECT_GT(WindowRedAt(window_->width() / 8, mid), kMidLow);
  EXPECT_LT(WindowRedAt(window_->width() / 8, mid), kMidHigh)
      << "the same progress is now an even blend of both scenes";
}

// A kind that is not one of the named values must be refused rather than
// falling through to the cross-fade: silently showing a different transition
// than the caller asked for is the exact failure a boolean return prevents.
TEST_F(ScenePresentTest, AnUnnamedTransitionKindIsRefused) {
  EXPECT_FALSE(presenter_->SetTransitionKind(
      static_cast<gl::TransitionKind>(/*not a kind=*/99)));
  EXPECT_EQ(gl::TransitionKind::kCrossFade, presenter_->transition_kind())
      << "a refused kind must leave the current one standing";
}

// The presenter's scene shares its two video programs with the single-video
// path (GlScene::CreateWithPrograms), and a uniform belongs to the PROGRAM, not
// to the path that set it. So a scene that placed a layer somewhere must not
// leave its rect behind for the next single-video frame to inherit.
//
// This is the regression test for that sharing: it fails if the single-video
// path sets uRect once at build time instead of every frame.
TEST_F(ScenePresentTest, APlacedSceneLayerDoesNotLeakItsRectIntoTheNextFrame) {
  GlScene* scene = presenter_->EnsureScene();
  ASSERT_TRUE(scene);
  const GlScene::LayerId layer = scene->AddVideoLayer(0);
  ASSERT_TRUE(layer != GlScene::kNoLayer);
  // The top quarter only, so the bottom of the window stays cleared to black.
  ASSERT_TRUE(scene->SetLayerRect(layer, 0.0f, 0.0f, 1.0f, 0.25f));

  base::scoped_refptr<VideoFrame> white = test::MakeSolidFrame(
      VideoFormat::kI420, kWindowSize, kWindowSize, test::kWhiteLuma,
      test::kNeutralChroma, test::kNeutralChroma);
  ASSERT_TRUE(white);
  ASSERT_TRUE(presenter_->UploadLayer(*scene, layer, *white));
  ASSERT_TRUE(presenter_->ComposeScene(*scene));
  presenter_->PresentComposed();
  ASSERT_GT(WindowTopRed(), test::kBrightThreshold) << "the rect did not apply";
  ASSERT_LT(WindowBottomRed(), test::kDarkThreshold)
      << "the rect should have covered only the top";

  // Same program, same frame: if uRect were still the scene's, the bottom
  // quarter of this frame would be missing.
  ASSERT_TRUE(presenter_->Compose(*white));
  presenter_->PresentComposed();
  EXPECT_GT(WindowBottomRed(), test::kBrightThreshold)
      << "a single-video frame must fill the canvas even after a placed layer";
  EXPECT_GT(WindowTopRed(), test::kBrightThreshold);
}

// UploadLayer ADDS into upload_ms. Assigning would report one layer's cost as
// the frame's, which is the reading that hides a scene getting expensive as
// layers are added -- so the accumulation is the contract, and this is it.
TEST_F(ScenePresentTest, UploadTimingAccumulatesAcrossLayers) {
  std::unique_ptr<GlScene> scene = NewScene();
  ASSERT_TRUE(scene);
  const GlScene::LayerId first = scene->AddVideoLayer(0);
  const GlScene::LayerId second = scene->AddVideoLayer(1);
  ASSERT_NE(first, GlScene::kNoLayer);
  ASSERT_NE(second, GlScene::kNoLayer);

  base::scoped_refptr<VideoFrame> frame = test::MakeSolidFrame(
      VideoFormat::kI420, kWindowSize, kWindowSize, test::kWhiteLuma,
      test::kNeutralChroma, test::kNeutralChroma);
  ASSERT_TRUE(frame);

  PresentTiming timing;
  ASSERT_TRUE(presenter_->UploadLayer(*scene, first, *frame, &timing));
  const double after_one = timing.upload_ms;
  EXPECT_GT(after_one, 0.0);
  EXPECT_EQ(timing.upload_path, UploadPath::kCpuSync);

  ASSERT_TRUE(presenter_->UploadLayer(*scene, second, *frame, &timing));
  EXPECT_GT(timing.upload_ms, after_one)
      << "a second layer's upload should add to the frame's upload time";
}

}  // namespace
}  // namespace avbase::media
