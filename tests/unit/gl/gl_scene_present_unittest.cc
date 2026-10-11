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

// Limited-range BT.601: 235 is white, 16 is black, 128 is neutral chroma. The
// colour matrix turns these into 255 and 0 on every channel.
constexpr uint8_t kWhiteLuma = 235;
constexpr uint8_t kBlackLuma = 16;
constexpr uint8_t kNeutralChroma = 128;

// Deliberately far apart: 0 vs 255 through a known matrix, so anything in
// between is a different bug than orientation or ordering.
constexpr int kBrightThreshold = 200;
constexpr int kDarkThreshold = 50;
// A cross-fade at half weight should land near the middle of those two, which
// is 127.5 -- so a band around it, wide enough for the 8-bit round trip and
// narrow enough that "showed one side only" misses it.
constexpr int kMidLow = 100;
constexpr int kMidHigh = 156;

// Keeps a bilinear sample from reaching one row past a half-way edge.
constexpr int kSampleInset = 4;

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
  int WindowTopRed() { return WindowRed(window_->height() - 1 - kSampleInset); }
  int WindowBottomRed() { return WindowRed(kSampleInset); }

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

  base::scoped_refptr<VideoFrame> frame = test::MakeTopHalfFrame(
      VideoFormat::kI420, kWindowSize, kWindowSize, kWhiteLuma, kBlackLuma);
  ASSERT_TRUE(frame);
  ASSERT_TRUE(presenter_->UploadLayer(*scene, layer, *frame));
  ASSERT_TRUE(presenter_->ComposeScene(*scene));
  presenter_->PresentComposed();

  EXPECT_GT(WindowTopRed(), kBrightThreshold)
      << "the top of the window should show the white half of the layer";
  EXPECT_LT(WindowBottomRed(), kDarkThreshold)
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

  base::scoped_refptr<VideoFrame> bright =
      test::MakeSolidFrame(VideoFormat::kI420, kWindowSize, kWindowSize,
                           kWhiteLuma, kNeutralChroma, kNeutralChroma);
  base::scoped_refptr<VideoFrame> dark =
      test::MakeSolidFrame(VideoFormat::kI420, kWindowSize, kWindowSize,
                           kBlackLuma, kNeutralChroma, kNeutralChroma);
  ASSERT_TRUE(bright);
  ASSERT_TRUE(dark);
  ASSERT_TRUE(presenter_->UploadLayer(*scene, white, *bright));
  ASSERT_TRUE(presenter_->UploadLayer(*scene, black, *dark));

  ASSERT_TRUE(presenter_->ComposeScene(*scene));
  presenter_->PresentComposed();
  EXPECT_LT(WindowTopRed(), kDarkThreshold)
      << "z=5 is on top of z=0, so the window should be black";

  // Same layers, same frames, one reorder -- and nothing re-uploaded, which is
  // the point of z being a field rather than a draw-order list.
  ASSERT_TRUE(scene->SetLayerZ(black, /*z=*/-5));
  ASSERT_TRUE(presenter_->ComposeScene(*scene));
  presenter_->PresentComposed();
  EXPECT_GT(WindowTopRed(), kBrightThreshold)
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

  base::scoped_refptr<VideoFrame> split = test::MakeTopHalfFrame(
      VideoFormat::kI420, kWindowSize, kWindowSize, kWhiteLuma, kBlackLuma);
  base::scoped_refptr<VideoFrame> dark =
      test::MakeSolidFrame(VideoFormat::kI420, kWindowSize, kWindowSize,
                           kBlackLuma, kNeutralChroma, kNeutralChroma);
  ASSERT_TRUE(split);
  ASSERT_TRUE(dark);
  ASSERT_TRUE(presenter_->UploadLayer(*from, from_layer, *split));
  ASSERT_TRUE(presenter_->UploadLayer(*to, to_layer, *dark));

  ASSERT_TRUE(presenter_->ComposeTransition(*from, *to, /*progress=*/0.0f));
  presenter_->PresentComposed();

  EXPECT_GT(WindowTopRed(), kBrightThreshold)
      << "progress 0 shows the from scene, and it must be upright";
  EXPECT_LT(WindowBottomRed(), kDarkThreshold);

  ASSERT_TRUE(presenter_->ComposeTransition(*from, *to, /*progress=*/1.0f));
  presenter_->PresentComposed();
  EXPECT_LT(WindowTopRed(), kDarkThreshold) << "progress 1 shows the to scene";
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

  base::scoped_refptr<VideoFrame> bright =
      test::MakeSolidFrame(VideoFormat::kI420, kWindowSize, kWindowSize,
                           kWhiteLuma, kNeutralChroma, kNeutralChroma);
  base::scoped_refptr<VideoFrame> dark =
      test::MakeSolidFrame(VideoFormat::kI420, kWindowSize, kWindowSize,
                           kBlackLuma, kNeutralChroma, kNeutralChroma);
  ASSERT_TRUE(bright);
  ASSERT_TRUE(dark);
  ASSERT_TRUE(presenter_->UploadLayer(*from, from_layer, *bright));
  ASSERT_TRUE(presenter_->UploadLayer(*to, to_layer, *dark));

  ASSERT_TRUE(presenter_->ComposeTransition(*from, *to, /*progress=*/0.5f));
  presenter_->PresentComposed();

  EXPECT_GT(WindowTopRed(), kMidLow);
  EXPECT_LT(WindowTopRed(), kMidHigh);
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

  base::scoped_refptr<VideoFrame> frame =
      test::MakeSolidFrame(VideoFormat::kI420, kWindowSize, kWindowSize,
                           kWhiteLuma, kNeutralChroma, kNeutralChroma);
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
