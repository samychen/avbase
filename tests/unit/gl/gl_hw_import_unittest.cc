// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The zero-copy path, checked the way every other drawing path here is: by
// reading the pixels back.
//
// WHY THIS FILE HAS TO EXIST. Import is the one upload route whose every call
// can succeed while the picture is wrong, and there are three ways for it to
// be wrong that no return code catches:
//
//   1. the imported texture is upside down (the rectangle target has no
//      GL_TEXTURE_2D's [0,1] coordinate space to hide the difference behind);
//   2. the planes are bound to the wrong sampler slots, so luma reads chroma;
//   3. the shader is the 2D one, which samples a rectangle texture as nothing.
//
// All three produce a successful CGL call, a successful upload and a
// successful draw. Only a pixel says otherwise -- which is the general rule
// this tree keeps relearning: a path with an orientation needs a test that
// reads the orientation back.
//
// WHAT IS REAL HERE AND WHAT IS NOT. The pixel buffer is a real
// IOSurface-backed CVPixelBuffer, so the import is the same call production
// makes -- not a stub standing in for one. What is not real is the decoder:
// nothing here decodes H.264. The frame is built the way VideoToolbox would
// build it and handed to the same code path, which is enough to prove the
// import and not enough to prove the pipeline; the pipeline end is covered by
// running play_glfw over a real file (see docs/PROGRESS.md).

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

#include "gl_test_context.h"
#include "gl_test_frames.h"
#include "gtest/gtest.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"
#include "render/gl/gl_api.h"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "platform/hwimport/hw_importer_factory.h"
#endif

namespace avbase::media {
namespace {

constexpr int kWindowSize = 64;

// Limited-range BT.601: 235 is white, 16 is black, 128 is neutral chroma.
constexpr uint8_t kWhiteLuma = 235;
constexpr uint8_t kBlackLuma = 16;
constexpr uint8_t kNeutralChroma = 128;

constexpr int kBrightThreshold = 200;
constexpr int kDarkThreshold = 50;
// Keeps a bilinear sample from reaching one row past a half-way edge.
constexpr int kSampleInset = 4;

#if defined(__APPLE__)
// A GPU-resident NV12 frame: an IOSurface-backed CVPixelBuffer of |w| x |h|,
// top half |y_top| and bottom half |y_bottom|, wrapped the way the FFmpeg
// VideoToolbox decoder wraps one.
//
// The IOSurface is what makes the import possible at all -- CGL can only bind
// a texture to a surface, and CVPixelBufferCreate only backs the buffer with
// one when kCVPixelBufferIOSurfacePropertiesKey is present. Asking for an
// empty dictionary is the whole trick: CoreVideo then allocates the backing
// itself, and there is no plane layout to get wrong by hand.
//
// |format| is a parameter only so one test can lie about it: the buffer is
// NV12 whatever is passed, and a frame that claims otherwise is how the
// importer's format gate gets exercised.
base::scoped_refptr<VideoFrame> MakeHardwareFrame(int w, int h, uint8_t y_top,
                                                  uint8_t y_bottom,
                                                  VideoFormat format) {
  CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CFMutableDictionaryRef io = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, io);
  CFRelease(io);
  CVPixelBufferRef buffer = nullptr;
  const CVReturn created = CVPixelBufferCreate(
      kCFAllocatorDefault, w, h,
      kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, attrs, &buffer);
  CFRelease(attrs);
  if (created != kCVReturnSuccess || buffer == nullptr) {
    return nullptr;
  }

  // Filled through the CPU mapping, which a hardware frame does not normally
  // offer -- but the buffer has not been published yet, and this is the only
  // way to give the test a picture it knows the value of.
  CVPixelBufferLockBaseAddress(buffer, 0);
  uint8_t* luma =
      static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
  const size_t luma_stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
  for (int row = 0; row < h && luma; ++row) {
    std::fill_n(luma + row * luma_stride, w, row < h / 2 ? y_top : y_bottom);
  }
  uint8_t* chroma =
      static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
  const size_t chroma_stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
  for (int row = 0; row < h / 2 && chroma; ++row) {
    uint8_t* dst = chroma + row * chroma_stride;
    for (int col = 0; col < w / 2; ++col) {
      dst[col * 2] = kNeutralChroma;
      dst[col * 2 + 1] = kNeutralChroma;
    }
  }
  CVPixelBufferUnlockBaseAddress(buffer, 0);

  NativeHandle handle;
  handle.kind = NativeHandleKind::kCVPixelBuffer;
  handle.id = buffer;
  const Size size{w, h};
  // No to_i420_cb: a frame that cannot be read back is the state this test is
  // about, and giving it one would let a failed import quietly take the CPU
  // route instead of failing.
  return VideoFrame::WrapNativeBuffer(
      handle, format, size, size, Rational{1, 1}, base::TimeDelta(),
      base::TimeDelta(), 0,
      base::BindOnce([](CVPixelBufferRef released) { CFRelease(released); },
                     buffer),
      base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
}
#endif  // defined(__APPLE__)

class HwImportTest : public ::testing::Test {
 protected:
  void SetUp() override {
#if !defined(__APPLE__)
    // Nothing to import through on this host: platform/hwimport returns no
    // backend, and the behaviour that replaces it -- a CPU readback -- is
    // correct by construction and tested elsewhere.
    GTEST_SKIP() << "no hardware-import backend on this platform";
#else
    std::string error;
    window_ = test::GlTestWindow::Create(kWindowSize, kWindowSize, &error);
    if (!window_) {
      GTEST_SKIP() << error;
    }
    importer_ = platform::hwimport::CreateHwTextureImporter();
    if (!importer_) {
      GTEST_SKIP() << "this host has no interop backend";
    }
    presenter_ = std::make_unique<GlPresenter>(window_->TakeContext());
    presenter_->SetHwTextureImporter(importer_.get());
    ASSERT_TRUE(presenter_->Init());
#endif
  }

#if defined(__APPLE__)
  // Red channel of the window's back buffer. Every frame here is neutral in
  // chroma, so one channel carries the whole signal. y is measured from the
  // bottom, as glReadPixels does.
  int WindowRed(int y) {
    return test::ReadPixel(*presenter_->api(), window_->width() / 2, y, GL_BACK)
        .r;
  }
  int WindowTopRed() { return WindowRed(window_->height() - 1 - kSampleInset); }
  int WindowBottomRed() { return WindowRed(kSampleInset); }

  std::unique_ptr<test::GlTestWindow> window_;
  std::unique_ptr<GlHwTextureImporter> importer_;
  std::unique_ptr<GlPresenter> presenter_;
#endif
};

#if defined(__APPLE__)

// The claim the whole path rests on: a frame whose pixels live on the GPU
// reaches the screen WITHOUT being copied, and says so. An import that
// silently fell back to a CPU upload would still draw the right picture and
// would still pass every pixel assertion below -- so the route is asserted
// here, on its own, before any colour is looked at.
TEST_F(HwImportTest, AnImportedFrameReportsTheZeroCopyRoute) {
  base::scoped_refptr<VideoFrame> frame = MakeHardwareFrame(
      kWindowSize, kWindowSize, kWhiteLuma, kWhiteLuma, VideoFormat::kNV12);
  ASSERT_TRUE(frame);
  EXPECT_FALSE(frame->IsMappable())
      << "the frame's pixels must be GPU-resident";

  PresentTiming timing;
  ASSERT_TRUE(presenter_->Compose(*frame, &timing));
  EXPECT_EQ(timing.upload_path, UploadPath::kHwZeroCopy);
}

// The orientation of an imported frame. A rectangle texture is addressed in
// texels, so nothing forces the coordinate space to agree with the 2D path's
// -- and a flip here is invisible to every call in the chain.
TEST_F(HwImportTest, AnImportedFrameLandsUpright) {
  base::scoped_refptr<VideoFrame> frame = MakeHardwareFrame(
      kWindowSize, kWindowSize, kWhiteLuma, kBlackLuma, VideoFormat::kNV12);
  ASSERT_TRUE(frame);

  ASSERT_TRUE(presenter_->Compose(*frame));
  presenter_->PresentComposed();

  EXPECT_GT(WindowTopRed(), kBrightThreshold)
      << "the white half of the frame is its top half";
  EXPECT_LT(WindowBottomRed(), kDarkThreshold);
}

// Both routes have to draw the same picture from the same numbers. They reach
// it through different texture targets and different fragment shaders -- the
// one thing that could plausibly disagree is a plane bound to the wrong
// sampler, and that shows up as the wrong value rather than as an error.
TEST_F(HwImportTest, BothRoutesDrawTheSameColour) {
  base::scoped_refptr<VideoFrame> gpu = MakeHardwareFrame(
      kWindowSize, kWindowSize, kWhiteLuma, kWhiteLuma, VideoFormat::kNV12);
  base::scoped_refptr<VideoFrame> cpu =
      test::MakeSolidFrame(VideoFormat::kNV12, kWindowSize, kWindowSize,
                           kWhiteLuma, kNeutralChroma, kNeutralChroma);
  ASSERT_TRUE(gpu);
  ASSERT_TRUE(cpu);

  ASSERT_TRUE(presenter_->Compose(*gpu));
  presenter_->PresentComposed();
  const int imported = WindowTopRed();

  ASSERT_TRUE(presenter_->Compose(*cpu));
  presenter_->PresentComposed();
  const int copied = WindowTopRed();

  EXPECT_NEAR(imported, copied, 2)
      << "an imported frame and a copied one with the same pixels should draw "
         "the same colour";
  EXPECT_GT(imported, kBrightThreshold);
}

// A GPU frame with no importer is REFUSED, not drawn from whatever the
// textures happen to hold. The alternative is a stale or black picture that
// looks like a decoder bug, so this is the failure the contract is written to
// produce -- and the reason a host without a backend reads the frame back in
// the sink instead of handing it here.
TEST_F(HwImportTest, AGpuFrameWithoutAnImporterIsRefused) {
  base::scoped_refptr<VideoFrame> frame = MakeHardwareFrame(
      kWindowSize, kWindowSize, kWhiteLuma, kWhiteLuma, VideoFormat::kNV12);
  ASSERT_TRUE(frame);

  // A SECOND window, because a context has one owner: presenter_ already made
  // this test's context current on this thread, and a second presenter cannot
  // have it. The point of the second presenter is the thing it does not do --
  // no SetHwTextureImporter() -- which is what a host with no interop backend
  // looks like from here.
  std::string error;
  std::unique_ptr<test::GlTestWindow> plain =
      test::GlTestWindow::Create(kWindowSize, kWindowSize, &error);
  ASSERT_TRUE(plain) << error;
  GlPresenter bare(plain->TakeContext());
  ASSERT_TRUE(bare.Init());

  EXPECT_FALSE(bare.Compose(*frame))
      << "a GPU frame with no importer must be refused, not drawn from "
         "whatever the plane textures happened to hold";
}

// Which layouts the backend will place. The buffer below is NV12 whatever the
// frame claims, and a claim of I420 is a frame the CGL backend cannot place --
// so the upload has to fail rather than sample the surface as if it were
// three planes.
TEST_F(HwImportTest, AFormatTheBackendCannotPlaceIsDeclined) {
  base::scoped_refptr<VideoFrame> frame = MakeHardwareFrame(
      kWindowSize, kWindowSize, kWhiteLuma, kWhiteLuma, VideoFormat::kI420);
  ASSERT_TRUE(frame);
  EXPECT_FALSE(importer_->CanImport(*frame))
      << "the backend places NV12; a frame declaring I420 is not one it can "
         "import";
  EXPECT_FALSE(presenter_->Compose(*frame))
      << "an import the backend declines must not be drawn";
}

// A scene holding both kinds of layer at once: one imported (rectangle
// textures) and one copied (2D textures). This is the case the per-layer
// program choice exists for, and it is the only place that choice is
// exercised -- a scene that picked ONE program for the whole frame would draw
// the other kind as nothing, and the symptom would be "a layer disappeared"
// rather than any error.
//
// Both directions are asserted, because "the top layer is visible" is true of
// two different bugs: a scene that drew only 2D layers and a scene that drew
// only rectangle ones. Hiding the top layer is what separates them.
TEST_F(HwImportTest, ASceneCanMixAnImportedLayerAndACopiedOne) {
  std::string error;
  std::unique_ptr<GlScene> scene = GlScene::Create(*presenter_->api(), &error);
  ASSERT_TRUE(scene) << error;
  const GlScene::LayerId imported = scene->AddVideoLayer(/*z=*/0);
  const GlScene::LayerId copied = scene->AddVideoLayer(/*z=*/1);
  ASSERT_NE(imported, GlScene::kNoLayer);
  ASSERT_NE(copied, GlScene::kNoLayer);

  base::scoped_refptr<VideoFrame> gpu = MakeHardwareFrame(
      kWindowSize, kWindowSize, kWhiteLuma, kWhiteLuma, VideoFormat::kNV12);
  base::scoped_refptr<VideoFrame> cpu =
      test::MakeSolidFrame(VideoFormat::kI420, kWindowSize, kWindowSize,
                           kBlackLuma, kNeutralChroma, kNeutralChroma);
  ASSERT_TRUE(gpu);
  ASSERT_TRUE(cpu);
  ASSERT_TRUE(presenter_->UploadLayer(*scene, imported, *gpu));
  ASSERT_TRUE(presenter_->UploadLayer(*scene, copied, *cpu));

  ASSERT_TRUE(presenter_->ComposeScene(*scene));
  presenter_->PresentComposed();
  EXPECT_LT(WindowTopRed(), kDarkThreshold)
      << "the copied layer is on top, so the window should be black";

  // Same scene, one layer hidden -- and the imported one is what is left. A
  // scene that only ever drew its 2D layers shows black here too, which is
  // exactly the bug this half is written to catch.
  ASSERT_TRUE(scene->SetLayerVisible(copied, false));
  ASSERT_TRUE(presenter_->ComposeScene(*scene));
  presenter_->PresentComposed();
  EXPECT_GT(WindowTopRed(), kBrightThreshold)
      << "with the copied layer hidden only the imported one is left";
}

// The backend names the target it binds, and it is not the one the CPU route
// uses -- which is the entire reason the video layer carries two fragment
// programs. Asserted as a value rather than left to a comment, because the
// consequence of the two sides disagreeing is a black picture, not an error.
TEST_F(HwImportTest, TheBackendNamesARectangleTarget) {
  EXPECT_EQ(importer_->target(), GL_TEXTURE_RECTANGLE);
  EXPECT_NE(importer_->target(), GL_TEXTURE_2D);
}

#endif  // defined(__APPLE__)

}  // namespace
}  // namespace avbase::media
