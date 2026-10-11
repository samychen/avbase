// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// What a multi-layer composition actually looks like, checked by reading the
// pixels back.
//
// Every property here is one that a compile error cannot catch and a GL error
// will not report: whether the higher z ended up on top, whether a half-opaque
// layer really blended, whether a rect landed where it was asked to. The
// values are therefore read out of the composed target rather than inferred
// from the calls that were made.

#include <limits>
#include <memory>
#include <string>

#include "gl_test_context.h"
#include "gl_test_frames.h"
#include "gtest/gtest.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_scene.h"
#include "render/gl/gl_api.h"
#include "render/gl/gl_framebuffer.h"

namespace avbase::media {
namespace {

using ::avbase::test::Describe;
using ::avbase::test::GlTestWindow;
using ::avbase::test::MakeSolidFrame;
using ::avbase::test::ReadPixel;
using ::avbase::test::Rgb;
using gl::GlApi;
using gl::GlFramebuffer;

constexpr int kCanvasSize = 64;

// Limited-range BT.601: 235 is white, 16 is black, chroma at 128 so the luma
// decision is the only thing the colour matrix can get wrong. The tests below
// compare brightness rather than hue for the same reason -- a hue comparison
// would be testing the matrix, which the plane-upload tests already do.
constexpr uint8_t kWhite = 235;
constexpr uint8_t kBlack = 16;
constexpr uint8_t kNeutral = 128;

// Red-flag bounds rather than tolerances: these are 0 vs 255 through a known
// matrix, so a mid value means a blend actually happened.
constexpr int kBright = 200;
constexpr int kDark = 60;

class GlSceneTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string error;
    window_ = GlTestWindow::Create(kCanvasSize, kCanvasSize, &error);
    if (!window_) {
      GTEST_SKIP() << error;
    }
    ASSERT_TRUE(window_->context()->MakeCurrent());
    api_ = std::make_unique<GlApi>();
    ASSERT_TRUE(api_->Load(window_->context(), &error)) << error;
    canvas_ = GlFramebuffer::Create(*api_);
    ASSERT_TRUE(canvas_);
    ASSERT_TRUE(canvas_->Ensure(kCanvasSize, kCanvasSize, &error)) << error;
    scene_ = GlScene::Create(*api_, &error);
    ASSERT_TRUE(scene_) << error;
  }

  void TearDown() override {
    // GL objects belong to the context; drop them while it is still current.
    scene_.reset();
    canvas_.reset();
    api_.reset();
  }

  // Adds a layer holding a frame of one flat colour.
  GlScene::LayerId AddSolid(int z, uint8_t luma) {
    const GlScene::LayerId id = scene_->AddVideoLayer(z);
    const base::scoped_refptr<VideoFrame> frame = MakeSolidFrame(
        VideoFormat::kI420, kCanvasSize, kCanvasSize, luma, kNeutral, kNeutral);
    EXPECT_TRUE(frame);
    EXPECT_TRUE(scene_->Upload(id, *frame));
    return id;
  }

  // Composes into the canvas and reads one pixel, in canvas coordinates with
  // y counted from the TOP so the assertions read the way the rects do.
  Rgb ComposeAndRead(int x, int y_from_top) {
    canvas_->Bind();
    scene_->Compose();
    // glReadPixels counts from the bottom.
    return ReadPixel(*api_, x, kCanvasSize - 1 - y_from_top);
  }

  std::unique_ptr<GlTestWindow> window_;
  std::unique_ptr<GlApi> api_;
  std::unique_ptr<GlFramebuffer> canvas_;
  std::unique_ptr<GlScene> scene_;
};

TEST_F(GlSceneTest, AZOrderDecidesWhichLayerIsOnTop) {
  const GlScene::LayerId under = AddSolid(/*z=*/0, kWhite);
  AddSolid(/*z=*/1, kBlack);

  const Rgb pixel = ComposeAndRead(kCanvasSize / 2, kCanvasSize / 2);
  EXPECT_LT(pixel.r, kDark)
      << "higher z should have won, got " << Describe(pixel);

  // The same two layers, the other way up: this is what makes the assertion
  // above about z rather than about the order they were added.
  ASSERT_TRUE(scene_->SetLayerZ(under, 2));
  const Rgb flipped = ComposeAndRead(kCanvasSize / 2, kCanvasSize / 2);
  EXPECT_GT(flipped.r, kBright)
      << "raising the white layer's z should have covered the black one, got "
      << Describe(flipped);
}

TEST_F(GlSceneTest, LayersSharingAZDrawInTheOrderTheyWereAdded) {
  // Both at z = 0, so only the insertion order can decide. A stable sort is
  // the difference between "the second one wins, predictably" and "whichever
  // the sort happened to leave last wins".
  AddSolid(/*z=*/0, kWhite);
  AddSolid(/*z=*/0, kBlack);
  const Rgb pixel = ComposeAndRead(kCanvasSize / 2, kCanvasSize / 2);
  EXPECT_LT(pixel.r, kDark)
      << "the later layer should be on top, got " << Describe(pixel);
}

TEST_F(GlSceneTest, AHiddenLayerDoesNotDraw) {
  AddSolid(/*z=*/0, kWhite);
  const GlScene::LayerId over = AddSolid(/*z=*/1, kBlack);

  Rgb pixel = ComposeAndRead(kCanvasSize / 2, kCanvasSize / 2);
  ASSERT_LT(pixel.r, kDark);

  ASSERT_TRUE(scene_->SetLayerVisible(over, false));
  pixel = ComposeAndRead(kCanvasSize / 2, kCanvasSize / 2);
  EXPECT_GT(pixel.r, kBright)
      << "hiding the top layer should reveal the one under it, got "
      << Describe(pixel);
}

TEST_F(GlSceneTest, OpacityBlendsTheUpperLayerWithTheLowerOne) {
  AddSolid(/*z=*/0, kBlack);
  const GlScene::LayerId over = AddSolid(/*z=*/1, kWhite);

  // Opaque first, so the "before" is the un-blended case.
  Rgb pixel = ComposeAndRead(kCanvasSize / 2, kCanvasSize / 2);
  ASSERT_GT(pixel.r, kBright);

  ASSERT_TRUE(scene_->SetLayerOpacity(over, 0.5f));
  pixel = ComposeAndRead(kCanvasSize / 2, kCanvasSize / 2);
  EXPECT_GT(pixel.r, 90) << "half opacity over black should be mid grey, got "
                         << Describe(pixel);
  EXPECT_LT(pixel.r, 170) << "half opacity over black should be mid grey, got "
                          << Describe(pixel);

  // And the endpoints still behave, which is what a broken clamp would break.
  ASSERT_TRUE(scene_->SetLayerOpacity(over, 0.0f));
  pixel = ComposeAndRead(kCanvasSize / 2, kCanvasSize / 2);
  EXPECT_LT(pixel.r, kDark)
      << "transparent should show the layer below, got " << Describe(pixel);
}

TEST_F(GlSceneTest, ARectPlacesTheLayerWhereItWasAsked) {
  AddSolid(/*z=*/0, kBlack);
  const GlScene::LayerId over = AddSolid(/*z=*/1, kWhite);
  // The left half of the target.
  ASSERT_TRUE(scene_->SetLayerRect(over, 0.0f, 0.0f, 0.5f, 1.0f));

  const Rgb left = ComposeAndRead(kCanvasSize / 4, kCanvasSize / 2);
  const Rgb right = ComposeAndRead(3 * kCanvasSize / 4, kCanvasSize / 2);
  EXPECT_GT(left.r, kBright) << "inside the rect, got " << Describe(left);
  EXPECT_LT(right.r, kDark) << "outside the rect, got " << Describe(right);
}

TEST_F(GlSceneTest, ARectIsRelativeToTheTopLeft) {
  AddSolid(/*z=*/0, kBlack);
  const GlScene::LayerId over = AddSolid(/*z=*/1, kWhite);
  // The top quarter. Normalised y counts from the top, and the fixture reads
  // with y from the top too, so a sign error here shows up as the layer
  // appearing at the bottom.
  ASSERT_TRUE(scene_->SetLayerRect(over, 0.0f, 0.0f, 1.0f, 0.25f));

  const Rgb top = ComposeAndRead(kCanvasSize / 2, kCanvasSize / 8);
  const Rgb bottom = ComposeAndRead(kCanvasSize / 2, 7 * kCanvasSize / 8);
  EXPECT_GT(top.r, kBright)
      << "the rect should cover the top, got " << Describe(top);
  EXPECT_LT(bottom.r, kDark)
      << "the rect should not cover the bottom, got " << Describe(bottom);
}

TEST_F(GlSceneTest, AStaleHandleAddressesNothing) {
  const GlScene::LayerId id = AddSolid(/*z=*/0, kWhite);
  ASSERT_TRUE(scene_->RemoveLayer(id));
  EXPECT_EQ(scene_->layer_count(), 0u);

  const base::scoped_refptr<VideoFrame> frame = MakeSolidFrame(
      VideoFormat::kI420, kCanvasSize, kCanvasSize, kBlack, kNeutral, kNeutral);
  ASSERT_TRUE(frame);
  // Every one of these would silently work if handles were vector indices.
  EXPECT_FALSE(scene_->Upload(id, *frame));
  EXPECT_FALSE(scene_->SetLayerOpacity(id, 0.5f));
  EXPECT_FALSE(scene_->SetLayerVisible(id, false));
  EXPECT_FALSE(scene_->SetLayerZ(id, 3));
  EXPECT_FALSE(scene_->RemoveLayer(id));
}

TEST_F(GlSceneTest, ANonFiniteRectIsRejected) {
  const GlScene::LayerId id = AddSolid(/*z=*/0, kWhite);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  // A caller that computed a NaN has a bug upstream; clamping would hide it.
  EXPECT_FALSE(scene_->SetLayerRect(id, 0.0f, 0.0f, 0.5f, nan));
  EXPECT_FALSE(scene_->SetLayerRect(id, nan, 0.0f, 1.0f, 1.0f));
  EXPECT_FALSE(scene_->SetLayerOpacity(id, nan));
  // The layer is untouched, so it still fills the target.
  const Rgb pixel = ComposeAndRead(kCanvasSize / 2, kCanvasSize / 2);
  EXPECT_GT(pixel.r, kBright) << "a rejected rect must not have been applied";
}

}  // namespace
}  // namespace avbase::media
