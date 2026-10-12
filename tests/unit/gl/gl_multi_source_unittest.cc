// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.
//
// Multi-source composition, checked the way every other GL property here is:
// by reading the pixels that reach the window.
//
// Two sources feeding two layers is the claim that cannot be checked by a
// return value. Sync() returns true for "I uploaded your frame" and true for
// "nothing arrived, keep the old picture", and both of those are correct in
// their own case -- so the only way to tell whether a second source really
// reached its own layer, and whether a source that went quiet really kept its
// picture, is to look at the composition.

#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "gl_test_context.h"
#include "gl_test_frames.h"
#include "gtest/gtest.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_layer_source.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"
#include "media/gl/gl_scene_engine.h"
#include "render/gl/gl_api.h"

namespace avbase::media {
namespace {

// The size asked of the window -- NOT the size of the drawable. On a HiDPI
// display GLFW hands back a framebuffer that is a whole multiple of this and
// the presenter's canvas follows the drawable, so a test that samples at
// coordinates derived from a hardcoded 64 collects every sample from the left
// fraction of a 128-wide canvas and sees one layer where there are two. Ask the
// window; see kDrawableIsNotTheRequestedSize.
constexpr int kRequestedSize = 64;

class MultiSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string error;
    window_ =
        test::GlTestWindow::Create(kRequestedSize, kRequestedSize, &error);
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

  // Red channel of one pixel of the current framebuffer. Every frame used
  // here is neutral in chroma, so all three channels carry the same value and
  // naming one of them is enough. |x| and |y| are in drawable pixels, and y is
  // measured from the bottom as glReadPixels does.
  int RedAt(int x, int y) {
    return test::ReadPixel(*presenter_->api(), x, y, GL_BACK).r;
  }
  // The two halves, named by where they are on screen. Both come from the
  // window rather than a constant, for the reason above.
  int LeftRed() { return RedAt(window_->width() / 4, window_->height() / 2); }
  int RightRed() {
    return RedAt(3 * window_->width() / 4, window_->height() / 2);
  }

  std::unique_ptr<test::GlTestWindow> window_;
  std::unique_ptr<GlPresenter> presenter_;
};

// The host hook the engine tests install: parks a source's layer on the right
// half so the sink's own frame stays visible on the left. It asks whether the
// layer exists because the engine creates it during Present(), not when the
// source is added.
void PlaceOnTheRightHalf(GlLayerSource* source, GlPresenter&, GlScene& scene,
                         GlScene::LayerId,
                         const base::scoped_refptr<VideoFrame>&,
                         PresentTiming*) {
  if (source->has_layer()) {
    scene.SetLayerRect(source->layer(), 0.5f, 0.0f, 0.5f, 1.0f);
  }
}

base::scoped_refptr<VideoFrame> SolidFrame(uint8_t luma) {
  return test::MakeSolidFrame(VideoFormat::kI420, kRequestedSize,
                              kRequestedSize, luma, test::kNeutralChroma,
                              test::kNeutralChroma);
}

// A GPU-resident frame -- not mappable, so the presenter has to decide the
// route -- that reports when its backing store goes away. The readback path is
// installed the way a real decoder installs it, so a host with no import
// backend takes the CPU-copy fallback and this test still holds.
base::scoped_refptr<VideoFrame> GpuFrame(bool* released) {
  NativeHandle handle;
  handle.kind = NativeHandleKind::kVaapiSurface;
  static int payload = 0;
  handle.id = &payload;
  const Size size{kRequestedSize, kRequestedSize};
  return VideoFrame::WrapNativeBuffer(
      handle, VideoFormat::kNV12, size, size, Rational{1, 1}, base::TimeDelta(),
      base::TimeDelta(), 0,
      base::BindOnce([](bool* out) { *out = true; }, released),
      base::BindRepeating(&SolidFrame, test::kBlackLuma));
}

// Two sources, two halves: if both were uploaded into one layer whichever ran
// last would own the whole window, and the losing half would be the wrong
// colour. That is the failure this exists to catch and no return code shows.
TEST_F(MultiSourceTest, TwoSourcesLandInTwoLayers) {
  std::unique_ptr<GlScene> scene = NewScene();
  ASSERT_TRUE(scene);

  GlLayerSource bright_source("bright", /*z=*/10);
  GlLayerSource dark_source("dark", /*z=*/20);
  bright_source.Publish(SolidFrame(test::kWhiteLuma));
  dark_source.Publish(SolidFrame(test::kBlackLuma));

  const GlScene::LayerId bright = bright_source.EnsureLayer(*scene);
  const GlScene::LayerId dark = dark_source.EnsureLayer(*scene);
  ASSERT_NE(bright, GlScene::kNoLayer);
  ASSERT_NE(dark, GlScene::kNoLayer);
  ASSERT_NE(bright, dark) << "each source must own its own layer";
  // Placed before the first upload, which is what EnsureLayer() being separate
  // from Sync() is for.
  ASSERT_TRUE(scene->SetLayerRect(bright, 0.0f, 0.0f, 0.5f, 1.0f));
  ASSERT_TRUE(scene->SetLayerRect(dark, 0.5f, 0.0f, 0.5f, 1.0f));

  ASSERT_TRUE(bright_source.Sync(*presenter_, *scene));
  ASSERT_TRUE(dark_source.Sync(*presenter_, *scene));
  ASSERT_TRUE(presenter_->ComposeScene(*scene));
  presenter_->PresentComposed();

  EXPECT_GT(LeftRed(), test::kBrightThreshold)
      << "the left half belongs to the bright source";
  EXPECT_LT(RightRed(), test::kDarkThreshold)
      << "the right half belongs to the dark source";
}

// A source that stops producing keeps showing the picture it last had. "No
// frame arrived" is a success, not a failure to draw -- and the frame is held
// because an imported one is still being sampled by the layer's textures.
TEST_F(MultiSourceTest, ASourceThatStopsKeepsItsLastPicture) {
  std::unique_ptr<GlScene> scene = NewScene();
  ASSERT_TRUE(scene);

  GlLayerSource source("stalling", /*z=*/0);
  source.Publish(SolidFrame(test::kWhiteLuma));
  const GlScene::LayerId layer = source.EnsureLayer(*scene);
  ASSERT_NE(layer, GlScene::kNoLayer);

  ASSERT_TRUE(source.Sync(*presenter_, *scene));
  ASSERT_TRUE(presenter_->ComposeScene(*scene));
  presenter_->PresentComposed();
  EXPECT_GT(LeftRed(), test::kBrightThreshold);
  EXPECT_EQ(source.frames_consumed(), 1u);

  // Nothing published since. The second Sync must not blank the layer -- and
  // must not re-upload, which is what the consumed count would show.
  ASSERT_TRUE(source.Sync(*presenter_, *scene));
  ASSERT_TRUE(presenter_->ComposeScene(*scene));
  presenter_->PresentComposed();
  EXPECT_GT(LeftRed(), test::kBrightThreshold)
      << "a stalled source keeps its picture rather than going black";
  EXPECT_EQ(source.frames_consumed(), 1u);
}

// WHY THE SOURCE KEEPS THE FRAME IT UPLOADED -- pinned by a test rather than by
// a comment, because the copy route cannot tell the difference.
//
// Copying a frame leaves the pixels in the texture, so dropping the reference
// afterwards is invisible. IMPORTING one does not: the layer's textures point
// at the decoder's own surface, and the frame's release callback is what frees
// it. A tidy-up that released the frame right after the upload would therefore
// pass every pixel test on this machine -- which has no import backend -- and
// produce garbage on one that does.
//
// So the assertion is on the release callback itself: it must NOT have run
// while the layer still shows the picture, and must run when the layer is let
// go. Note the source holds the frame it was HANDED, not the copy the fallback
// uploaded -- which is exactly the frame whose surface is the decoder's.
TEST_F(MultiSourceTest, ASourceHoldsTheFrameItsLayerIsShowing) {
  std::unique_ptr<GlScene> scene = NewScene();
  ASSERT_TRUE(scene);

  bool released = false;
  base::scoped_refptr<VideoFrame> frame = GpuFrame(&released);
  ASSERT_TRUE(frame);

  GlLayerSource source("holding", /*z=*/0);
  source.Publish(frame);
  frame = nullptr;  // The source's reference is now the only one left.
  ASSERT_FALSE(released);

  ASSERT_TRUE(source.Sync(*presenter_, *scene));
  EXPECT_FALSE(released)
      << "the layer is still sampling this frame's surface; dropping the "
         "reference here would free it while it is being read";

  source.Detach(scene.get());
  EXPECT_TRUE(released)
      << "letting the layer go is what releases the frame it was showing";
}

// The engine: the sink's own frame is the main layer, a source the host added
// is composited over it, and removing that source takes its layer away again.
// The placement has to happen through the update hook, because the engine
// creates a source's layer on the GL thread during Present().
TEST_F(MultiSourceTest, TheEngineComposesTheMainFrameAndASource) {
  GlSceneEngine engine;
  std::shared_ptr<GlLayerSource> source = engine.AddSource("second", /*z=*/10);
  ASSERT_TRUE(source);
  engine.SetUpdateCallback(base::BindRepeating(&PlaceOnTheRightHalf,
                                               base::Unretained(source.get())));

  source->Publish(SolidFrame(test::kBlackLuma));
  base::scoped_refptr<VideoFrame> main_frame = SolidFrame(test::kWhiteLuma);
  ASSERT_TRUE(main_frame);
  // Compose() rather than Present(): the swap at the end of Present()
  // leaves the back buffer undefined, and the pixels are the claim.
  ASSERT_TRUE(engine.Compose(*presenter_, main_frame));
  presenter_->PresentComposed();

  EXPECT_GT(LeftRed(), test::kBrightThreshold)
      << "the left half is the sink's own frame";
  EXPECT_LT(RightRed(), test::kDarkThreshold)
      << "the right half is the second source, drawn over it";

  // Retiring the source gives the layer back on the GL thread: the next
  // present shows the main frame everywhere.
  ASSERT_TRUE(engine.RemoveSource(source));
  ASSERT_EQ(engine.source_count(), 0u);
  // Compose() rather than Present(): the swap at the end of Present()
  // leaves the back buffer undefined, and the pixels are the claim.
  ASSERT_TRUE(engine.Compose(*presenter_, main_frame));
  presenter_->PresentComposed();
  EXPECT_GT(RightRed(), test::kBrightThreshold)
      << "a removed source must stop covering the frame underneath it";
}

// A source published between two presents shows up at the next one, which is
// the whole point of the mailbox: the producer's clock and the display's are
// not the same clock.
TEST_F(MultiSourceTest, AFramePublishedBetweenPresentsShowsAtTheNextOne) {
  GlSceneEngine engine;
  std::shared_ptr<GlLayerSource> source = engine.AddSource("late", /*z=*/10);
  ASSERT_TRUE(source);
  engine.SetUpdateCallback(base::BindRepeating(&PlaceOnTheRightHalf,
                                               base::Unretained(source.get())));

  base::scoped_refptr<VideoFrame> main_frame = SolidFrame(test::kWhiteLuma);
  // Compose() rather than Present(): the swap at the end of Present()
  // leaves the back buffer undefined, and the pixels are the claim.
  ASSERT_TRUE(engine.Compose(*presenter_, main_frame));
  presenter_->PresentComposed();
  EXPECT_GT(RightRed(), test::kBrightThreshold)
      << "nothing published yet: the main frame shows through";

  source->Publish(SolidFrame(test::kBlackLuma));
  ASSERT_EQ(source->frames_published(), 1u);
  // Compose() rather than Present(): the swap at the end of Present()
  // leaves the back buffer undefined, and the pixels are the claim.
  ASSERT_TRUE(engine.Compose(*presenter_, main_frame));
  presenter_->PresentComposed();
  EXPECT_LT(RightRed(), test::kDarkThreshold)
      << "the frame that arrived between presents shows at the next one";
  EXPECT_EQ(source->frames_consumed(), 1u);
}

}  // namespace
}  // namespace avbase::media
