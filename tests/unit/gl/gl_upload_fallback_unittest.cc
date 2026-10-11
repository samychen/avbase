// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// What happens to a GPU-resident frame the import route will not take.
//
// WHY THIS FILE HAS TO EXIST. This is the one upload decision that used to be
// made in two places at once: each sink read a hardware frame back before the
// present, and then read it back AGAIN if the import was refused during it,
// with its own sticky flag deciding whether to try. Two answers to "is this
// frame read back or dropped" is two places to be wrong, and the two were not
// the same code. The decision now lives in GlVideoLayer::Upload(), and what
// needs proving is the part a return code does not cover:
//
//   1. a refused import still DRAWS -- via a readback, on the CPU route;
//   2. it draws the right way up, which no call in the chain reports;
//   3. the layer ends up back on GL_TEXTURE_2D textures, because the failed
//      import moved it to the backend's target first;
//   4. the refusal is sticky for the backend that refused, and only for that
//      one -- a backend that declines a FORMAT is not a driver refusal, and
//      poisoning the layer over a format would cost every frame after it.
//
// WHAT IS REAL HERE AND WHAT IS NOT. The GL is real: a real context, real
// textures, real pixels read back. The backend is not -- it is a fake that
// accepts and then refuses, which is precisely the shape no real backend on
// this host can be made to take on demand. That is the right trade here: what
// is under test is the layer's decision, and the real import is covered by
// gl_hw_import_unittest.cc against a real IOSurface.

#include <cstdint>
#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "gl_test_context.h"
#include "gl_test_frames.h"
#include "gtest/gtest.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_hw_texture_importer.h"
#include "media/gl/gl_present.h"

namespace avbase::media {
namespace {

constexpr int kWindowSize = 64;

// Says yes and then declines: CanImport() accepts the frame and ImportPlane()
// refuses it. That is the state a real backend reaches when the interop API
// answers an error for a surface it claimed to support -- the case that used
// to be caught, if at all, one layer above the one that knew about it.
class RefusingImporter : public GlHwTextureImporter {
 public:
  // RECTANGLE, like the real macOS backend: it forces the layer to switch
  // texture sets before it refuses, so the fallback has to switch BACK. A fake
  // that answered GL_TEXTURE_2D would leave the fallback unable to prove that.
  unsigned int target() const override { return GL_TEXTURE_RECTANGLE; }

  bool CanImport(const VideoFrame& /*frame*/) const override {
    ++can_import_calls_;
    return accepts_;
  }

  bool ImportPlane(const VideoFrame& /*frame*/, int /*index*/,
                   const PlaneUploadLayout& /*layout*/) override {
    ++import_plane_calls_;
    return false;
  }

  const char* name() const override { return "test-refusing"; }

  int can_import_calls() const { return can_import_calls_; }
  int import_plane_calls() const { return import_plane_calls_; }
  void set_accepts(bool accepts) { accepts_ = accepts; }

 private:
  bool accepts_{true};
  mutable int can_import_calls_{0};
  int import_plane_calls_{0};
};

// A GPU-resident frame: not mappable, so Upload() cannot take the copy route
// directly. |readback| is whether the producer installed a readback path --
// the difference between "expensive" and "nothing to draw at all".
base::scoped_refptr<VideoFrame> MakeGpuFrame(bool readback) {
  NativeHandle handle;
  // Any typed kind: WrapNativeBuffer() refuses kNone, and nothing here reads
  // the handle -- the only backend in this test is the fake above, which never
  // looks at it. Using a real CVPixelBuffer instead would make these cases
  // macOS-only for no gain: what is under test is the layer's decision, and the
  // real import has its own file.
  handle.kind = NativeHandleKind::kVaapiSurface;
  handle.id = &handle;
  const Size size{kWindowSize, kWindowSize};
  base::RepeatingCallback<base::scoped_refptr<VideoFrame>()> to_i420;
  if (readback) {
    // White top half, black bottom half: the shape that makes orientation
    // assertable, and the same shape every other pixel test here uses.
    to_i420 = base::BindRepeating([]() -> base::scoped_refptr<VideoFrame> {
      return test::MakeTopHalfFrame(VideoFormat::kI420, kWindowSize,
                                    kWindowSize, test::kWhiteLuma,
                                    test::kBlackLuma);
    });
  }
  return VideoFrame::WrapNativeBuffer(
      handle, VideoFormat::kNV12, size, size, Rational{1, 1}, base::TimeDelta(),
      base::TimeDelta(), 0,
      // Nothing to release: this frame's backing store is a test fixture, not a
      // producer's. WrapNativeBuffer() insists on a callback anyway, because a
      // frame without one is how a real producer leaks its surface.
      base::BindOnce([] {}), std::move(to_i420));
}

class UploadFallbackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string error;
    window_ = test::GlTestWindow::Create(kWindowSize, kWindowSize, &error);
    if (!window_) {
      GTEST_SKIP() << error;
    }
    presenter_ = std::make_unique<GlPresenter>(window_->TakeContext());
    presenter_->SetHwTextureImporter(&importer_);
    ASSERT_TRUE(presenter_->Init());
  }

  // Red channel of the window's back buffer. Every frame here is neutral in
  // chroma, so one channel carries the whole signal. y is measured from the
  // bottom, as glReadPixels does.
  int WindowRed(int y) {
    return test::ReadPixel(*presenter_->api(), window_->width() / 2, y, GL_BACK)
        .r;
  }
  int WindowTopRed() {
    return WindowRed(window_->height() - 1 - test::kSampleInset);
  }
  int WindowBottomRed() { return WindowRed(test::kSampleInset); }

  std::unique_ptr<test::GlTestWindow> window_;
  std::unique_ptr<GlPresenter> presenter_;
  RefusingImporter importer_;
};

// The claim the whole refactor rests on: a frame the driver will not import is
// still displayed, and says how. An upload that returned false here would have
// the sink report a submit failure for a frame it could perfectly well have
// drawn.
TEST_F(UploadFallbackTest, ARefusedImportStillDrawsOnTheCpuRoute) {
  base::scoped_refptr<VideoFrame> frame = MakeGpuFrame(/*readback=*/true);
  ASSERT_TRUE(frame);
  EXPECT_FALSE(frame->IsMappable());

  PresentTiming timing;
  ASSERT_TRUE(presenter_->Compose(*frame, &timing));
  EXPECT_EQ(timing.upload_path, UploadPath::kCpuSync);
  EXPECT_TRUE(importer_.import_plane_calls() > 0)
      << "the import route must have been tried before falling back";
}

// The part no return value covers. The layer moved to the backend's RECTANGLE
// target when it tried to import, so the fallback has to move it back and draw
// with the 2D program -- a layer left on rectangle textures with the 2D
// program draws nothing at all, which every call in that chain reports as
// success.
TEST_F(UploadFallbackTest, TheFallbackDrawsTheFrameUpright) {
  base::scoped_refptr<VideoFrame> frame = MakeGpuFrame(/*readback=*/true);
  ASSERT_TRUE(frame);

  ASSERT_TRUE(presenter_->Compose(*frame));
  presenter_->PresentComposed();

  EXPECT_GT(WindowTopRed(), test::kBrightThreshold)
      << "the white half of the frame is its top half";
  EXPECT_LT(WindowBottomRed(), test::kDarkThreshold);
}

// Sticky, because a driver that refuses this API refuses it for every frame
// after the first: asking again would spend a failed import per frame to
// arrive at the same picture.
TEST_F(UploadFallbackTest, TheRefusalIsStickyForTheBackendThatRefused) {
  base::scoped_refptr<VideoFrame> frame = MakeGpuFrame(/*readback=*/true);
  ASSERT_TRUE(frame);

  ASSERT_TRUE(presenter_->Compose(*frame));
  const int after_first = importer_.can_import_calls();
  EXPECT_EQ(after_first, 1);

  ASSERT_TRUE(presenter_->Compose(*frame));
  EXPECT_EQ(importer_.can_import_calls(), after_first)
      << "a backend that already refused must not be asked again";
}

// Keyed to the backend rather than being a bare flag, so installing a
// different one gets a fresh chance without anybody having to remember to
// clear anything -- which is exactly what the sinks' bools required.
TEST_F(UploadFallbackTest, ADifferentBackendGetsAFreshChance) {
  base::scoped_refptr<VideoFrame> frame = MakeGpuFrame(/*readback=*/true);
  ASSERT_TRUE(frame);
  ASSERT_TRUE(presenter_->Compose(*frame));

  RefusingImporter other;
  presenter_->SetHwTextureImporter(&other);
  ASSERT_TRUE(presenter_->Compose(*frame));

  EXPECT_EQ(other.can_import_calls(), 1)
      << "a new backend deserves the first frame it refused nothing on";
}

// Declining a FORMAT is not a driver refusal, and must not cost every frame
// after it: a stream that opens on a P010 keyframe and continues in NV12 would
// otherwise be read back for its whole length.
TEST_F(UploadFallbackTest, AFrameTheBackendDeclinesDoesNotPoisonTheLayer) {
  importer_.set_accepts(false);
  base::scoped_refptr<VideoFrame> frame = MakeGpuFrame(/*readback=*/true);
  ASSERT_TRUE(frame);

  ASSERT_TRUE(presenter_->Compose(*frame));
  EXPECT_EQ(importer_.can_import_calls(), 1);
  EXPECT_EQ(importer_.import_plane_calls(), 0)
      << "a declined format must not reach ImportPlane() at all";

  importer_.set_accepts(true);
  ASSERT_TRUE(presenter_->Compose(*frame));
  EXPECT_EQ(importer_.can_import_calls(), 2)
      << "the next frame has to be asked about again";
}

// A GPU frame on a host with no interop backend at all: the same readback, not
// a failure. This is the case that used to be handled only in the sinks, and
// only because they happened to ask before presenting.
TEST_F(UploadFallbackTest, AGpuFrameWithoutABackendAlsoFallsBack) {
  presenter_->SetHwTextureImporter(nullptr);
  base::scoped_refptr<VideoFrame> frame = MakeGpuFrame(/*readback=*/true);
  ASSERT_TRUE(frame);

  PresentTiming timing;
  ASSERT_TRUE(presenter_->Compose(*frame, &timing));
  EXPECT_EQ(timing.upload_path, UploadPath::kCpuSync);
}

// The one frame that genuinely cannot be drawn: GPU-resident with no readback
// path installed. Reading it back is impossible and pretending otherwise would
// draw stale textures, so this is a failure the sink can report.
TEST_F(UploadFallbackTest, AFrameThatCannotBeReadBackIsReportedNotDrawn) {
  base::scoped_refptr<VideoFrame> frame = MakeGpuFrame(/*readback=*/false);
  ASSERT_TRUE(frame);
  EXPECT_FALSE(frame->ToI420());

  EXPECT_FALSE(presenter_->Compose(*frame));
}

}  // namespace
}  // namespace avbase::media
