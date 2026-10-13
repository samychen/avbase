// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.
//
// The three layer attributes that were missing: the scene's background, a
// layer's source rectangle (crop), and how a picture is fitted into its rect.
//
// Why this file is made of pixels rather than return codes: every one of the
// three is a property whose wrong answer is a picture that still draws. A crop
// that does nothing and a crop that works are the same `true`. A background
// that was never set and one that was set to black are the same pixels -- which
// is exactly why "opaque black" cannot be the test colour for §1.
//
// One trap is worth naming up front: crop-then-rotate and rotate-then-crop
// produce the SAME picture on an upright clip, so the order only separates on
// material that carries a quarter turn. §3 is therefore written against a
// rotated frame, not an upright one.

#include <limits>
#include <memory>
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
using ::avbase::test::MakeSolidFrame;
using ::avbase::test::MakeTopHalfFrame;
using ::avbase::test::MakeTopHalfFrameRotated;
using ::avbase::test::ReadPixel;
using ::avbase::test::Rgb;
using gl::GlApi;

constexpr int kRequestedSize = 64;

class LayerAttributeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string error;
    window_ = GlTestWindow::Create(kRequestedSize, kRequestedSize, &error);
    if (!window_) {
      GTEST_SKIP() << error;
    }
    presenter_ = std::make_unique<GlPresenter>(window_->TakeContext());
    ASSERT_TRUE(presenter_->Init());
    scene_ = GlScene::Create(*presenter_->api(), &error);
    ASSERT_TRUE(scene_) << error;
  }

  void TearDown() override {
    scene_.reset();
    presenter_.reset();
  }

  // Composes the scene into the canvas and blits it; returns false if the
  // compose itself failed. Stops before the swap so the back buffer is still
  // defined -- see gl_present.h.
  bool Compose() { return presenter_->ComposeScene(*scene_); }

  // Red channel of one pixel, addressed the way the rects are: y from the TOP.
  int Red(int x, int y_from_top) {
    return ReadPixel(*presenter_->api(), x, window_->height() - 1 - y_from_top,
                     GL_BACK)
        .r;
  }

  // One layer filled with a frame of |w| x |h|, all white.
  GlScene::LayerId AddWhiteLayer(int w, int h) {
    const GlScene::LayerId id = scene_->AddVideoLayer(0);
    EXPECT_NE(id, GlScene::kNoLayer);
    if (id == GlScene::kNoLayer) {
      return id;
    }
    base::scoped_refptr<VideoFrame> frame = MakeSolidFrame(
        VideoFormat::kI420, w, h, kWhiteLuma, kNeutralChroma, kNeutralChroma);
    EXPECT_TRUE(frame);
    EXPECT_TRUE(scene_->Upload(id, *frame));
    return id;
  }

  // One layer holding a frame whose top half is white and bottom half black.
  GlScene::LayerId AddTopHalfLayer(int rotation = 0) {
    const GlScene::LayerId id = scene_->AddVideoLayer(0);
    EXPECT_NE(id, GlScene::kNoLayer);
    if (id == GlScene::kNoLayer) {
      return id;
    }
    base::scoped_refptr<VideoFrame> frame = MakeTopHalfFrameRotated(
        VideoFormat::kI420, window_->width(), window_->height(), kWhiteLuma,
        kBlackLuma, rotation);
    EXPECT_TRUE(frame);
    EXPECT_TRUE(scene_->Upload(id, *frame));
    return id;
  }

  int w() const { return window_->width(); }
  int h() const { return window_->height(); }

  std::unique_ptr<GlTestWindow> window_;
  std::unique_ptr<GlPresenter> presenter_;
  std::unique_ptr<GlScene> scene_;
};

// ---- §1 The background -----------------------------------------------------

// White rather than the default: black is what an unset background already is,
// so asserting on it would pass just as well with the setter stubbed out.
TEST_F(LayerAttributeTest, TheBackgroundShowsWhereNoLayerCovers) {
  ASSERT_TRUE(Compose());
  EXPECT_LT(Red(w() / 2, h() / 2), kDarkThreshold) << "the default is black";

  ASSERT_TRUE(scene_->SetBackgroundColor(1.0f, 1.0f, 1.0f, 1.0f));
  ASSERT_TRUE(Compose());
  EXPECT_GT(Red(w() / 2, h() / 2), kBrightThreshold)
      << "an empty scene shows its background";
}

// ---- §2 The source rectangle (crop) -----------------------------------------

// A crop keeps the part it names and drops the rest: ONE frame, drawn twice,
// comes out white the first time and black the second. White is on top in the
// stored picture, so "top half" and "bottom half" are different assertions
// about the same pixels -- which is what makes this about the crop.
TEST_F(LayerAttributeTest, ASourceRectDrawsOnlyThePartItNames) {
  const GlScene::LayerId layer = AddTopHalfLayer();
  ASSERT_NE(layer, GlScene::kNoLayer);

  ASSERT_TRUE(scene_->SetLayerSourceRect(layer, 0.0f, 0.0f, 1.0f, 0.5f));
  ASSERT_TRUE(Compose());
  EXPECT_GT(Red(w() / 2, h() / 2), kBrightThreshold) << "top half is white";

  ASSERT_TRUE(scene_->SetLayerSourceRect(layer, 0.0f, 0.5f, 1.0f, 0.5f));
  ASSERT_TRUE(Compose());
  EXPECT_LT(Red(w() / 2, h() / 2), kDarkThreshold) << "bottom half is black";
}

// Cropping to a quarter and sampling inside it is not enough on its own --
// with a uniform colour the crop is invisible. The point of §2's two-sided
// assertion is that the SAME frame yields two different pictures.

// ---- §3 Crop is applied BEFORE the display rotation -------------------------

// The two orders agree on an upright clip and disagree on a turned one -- which
// is why this test exists at all, and why it is written against rotation = 90.
//
// The frame is stored white-on-top; a quarter turn carries that white half to
// the RIGHT. Cropping to the stored picture's top half therefore selects the
// white half however the clip is turned, and the whole rect comes out white --
// including the LEFT quarter, which belongs to the black half once the picture
// has been turned. An implementation that rotated first and then cropped would
// be taking the top half of what is on screen, whose left sample is black.
TEST_F(LayerAttributeTest, CroppingNamesTheStoredPictureNotTheTurnedOne) {
  const GlScene::LayerId layer = AddTopHalfLayer(/*rotation=*/90);
  ASSERT_NE(layer, GlScene::kNoLayer);
  EXPECT_EQ(90, scene_->LayerRotation(layer));

  ASSERT_TRUE(scene_->SetLayerSourceRect(layer, 0.0f, 0.0f, 1.0f, 0.5f));
  ASSERT_TRUE(Compose());
  EXPECT_GT(Red(w() / 4, h() / 2), kBrightThreshold)
      << "the crop picked the white half, which the turn then took left";
  EXPECT_GT(Red(3 * w() / 4, h() / 2), kBrightThreshold);
}

// ---- §4 Fitting a picture into its rect ------------------------------------

// The default: the picture covers its rect whatever its own shape is. A 2:1
// frame in a square rect therefore reaches the corners, where a fit would
// have left background.
TEST_F(LayerAttributeTest, StretchFillsTheRectWhateverThePicturesShape) {
  const GlScene::LayerId layer = AddWhiteLayer(w(), h() / 2);
  ASSERT_NE(layer, GlScene::kNoLayer);

  ASSERT_TRUE(Compose());
  EXPECT_GT(Red(kSampleInset, kSampleInset), kBrightThreshold);
  EXPECT_GT(Red(w() - 1 - kSampleInset, h() - 1 - kSampleInset),
            kBrightThreshold);
}

// Fit, the other axis: a 1:2 frame in a square rect leaves the background on
// the LEFT and RIGHT instead. Both this and the letterbox case are asserted
// on the corners because that is where a fit is distinguishable from a
// stretch -- a fit only ever uncovers the EDGE of a rect, never its middle.
TEST_F(LayerAttributeTest, FitNarrowsTheAxisThatDoesNotFit) {
  const GlScene::LayerId layer = AddWhiteLayer(w() / 2, h());
  ASSERT_NE(layer, GlScene::kNoLayer);
  ASSERT_TRUE(scene_->SetLayerContentMode(layer, LayerContentMode::kFit));

  ASSERT_TRUE(Compose());
  EXPECT_GT(Red(w() / 2, h() / 2), kBrightThreshold)
      << "the band is the picture";
  EXPECT_LT(Red(kSampleInset, h() / 2), kDarkThreshold)
      << "left of the band is background";
  EXPECT_LT(Red(w() - 1 - kSampleInset, h() / 2), kDarkThreshold)
      << "right of the band is background";
}

// The crop is part of what gets fitted. Cutting the stored 2:1 picture down to
// its left half makes the region square, so it fills the square rect instead of
// being letterboxed; an implementation that fitted the UNCROPPED frame would
// leave bars around a region the caller had already narrowed.
TEST_F(LayerAttributeTest, FittingUsesTheCroppedRegionNotTheWholeFrame) {
  const GlScene::LayerId layer = AddWhiteLayer(w(), h() / 2);
  ASSERT_NE(layer, GlScene::kNoLayer);
  ASSERT_TRUE(scene_->SetLayerContentMode(layer, LayerContentMode::kFit));
  ASSERT_TRUE(scene_->SetLayerSourceRect(layer, 0.0f, 0.0f, 0.5f, 1.0f));

  ASSERT_TRUE(Compose());
  EXPECT_GT(Red(kSampleInset, kSampleInset), kBrightThreshold);
  EXPECT_GT(Red(w() - 1 - kSampleInset, h() - 1 - kSampleInset),
            kBrightThreshold);
}

// ---- §5 Bad values ----------------------------------------------------------

// A crop has to name a region that exists. Non-finite, empty, or reaching
// outside the picture are all refused, and the previous crop keeps standing --
// a layer must not be left showing an empty box because a caller passed NaN.
TEST_F(LayerAttributeTest, ASourceRectThatIsNotARegionIsRejected) {
  const GlScene::LayerId layer = AddTopHalfLayer();
  ASSERT_NE(layer, GlScene::kNoLayer);

  // A crop that is verifiably in effect: the top quarter, which is white.
  ASSERT_TRUE(scene_->SetLayerSourceRect(layer, 0.0f, 0.0f, 0.25f, 1.0f));
  ASSERT_TRUE(Compose());
  EXPECT_GT(Red(w() / 2, h() / 8), kBrightThreshold) << "inside the crop";

  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  EXPECT_FALSE(scene_->SetLayerSourceRect(layer, 0.0f, 0.0f, 0.5f, nan));
  EXPECT_FALSE(scene_->SetLayerSourceRect(layer, nan, 0.0f, 1.0f, 1.0f));
  EXPECT_FALSE(scene_->SetLayerSourceRect(layer, 0.0f, 0.0f, 0.0f, 1.0f));
  EXPECT_FALSE(scene_->SetLayerSourceRect(layer, 0.0f, 0.0f, -0.5f, 1.0f));
  EXPECT_FALSE(scene_->SetLayerSourceRect(layer, -0.25f, 0.0f, 0.5f, 1.0f));
  EXPECT_FALSE(scene_->SetLayerSourceRect(layer, 0.75f, 0.0f, 0.5f, 1.0f));
  EXPECT_FALSE(scene_->SetLayerSourceRect(layer, 0.0f, 0.75f, 1.0f, 0.5f));
  EXPECT_FALSE(scene_->SetLayerSourceRect(layer, 0.0f, inf, 1.0f, 0.5f));

  // Untouched: still the top quarter, so still white.
  ASSERT_TRUE(Compose());
  EXPECT_GT(Red(w() / 2, h() / 8), kBrightThreshold);
}

// Same policy for the background. The colour that gets asserted is NOT the
// default, for the reason §1's comment gives.
TEST_F(LayerAttributeTest, ANonFiniteBackgroundIsRejected) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(scene_->SetBackgroundColor(nan, 0.0f, 0.0f, 1.0f));
  EXPECT_FALSE(scene_->SetBackgroundColor(0.0f, 0.0f, nan, 1.0f));
  EXPECT_TRUE(scene_->SetBackgroundColor(1.0f, 1.0f, 1.0f, 1.0f));

  // The last good value stands.
  ASSERT_TRUE(Compose());
  EXPECT_GT(Red(w() / 2, h() / 2), kBrightThreshold);
}

}  // namespace
}  // namespace avbase::media
