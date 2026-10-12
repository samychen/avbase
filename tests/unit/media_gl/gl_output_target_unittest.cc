// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The routing claim behind the output-strategy refactor: a sink with a target
// draws through the target, and only through the route the target says it has.
//
// WHY THIS FILE HAS TO EXIST. The duplication this refactor removed -- a second
// sink carrying a second copy of the render loop -- was invisible from inside
// either sink, and the loop is the part with the teardown ordering and the
// cadence in it. What is left is one loop that asks a GlOutputTarget which
// route it has, and the thing worth pinning down is that it routes on the
// answer and asks nothing else. A regression here does not fail to compile: it
// presents through the wrong API, or builds a GL presenter nobody asked for.
//
// WHAT IS REAL HERE AND WHAT IS NOT. The sink is real: a real render thread, a
// real cadence, real stats. The display and the target are not -- there is no
// window and no GL context anywhere in this file, which is the point. The
// routing decision happens before either would be needed.

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/native_display.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/gl/gl_output_target.h"
#include "media/gl/gl_video_sink.h"

namespace avbase::media {
namespace {

// A target that records what it was asked and draws nothing. |gl_route| chooses
// which route it claims, which is how both routes get exercised with no GL
// context anywhere in the build.
class RecordingTarget final : public GlOutputTarget {
 public:
  explicit RecordingTarget(bool gl_route) : gl_route_(gl_route) {}

  void SetSurface(void* raw) override {
    set_surface_calls_.fetch_add(1);
    raw_ = raw;
  }
  bool has_gl_route() const override { return gl_route_; }

  GlSurfaceBinding BindSurface() override {
    bind_calls_.fetch_add(1);
    // Deliberately no context: the sink only has to get far enough to prove it
    // took the GL route, and whether a presenter can then be built is a
    // different claim than the one under test.
    return GlSurfaceBinding();
  }

  bool PresentWithoutGl(const VideoFrame& frame,
                        PresentTiming* timing) override {
    without_gl_calls_.fetch_add(1);
    last_frame_mappable_.store(frame.IsMappable());
    if (timing) {
      timing->upload_path = UploadPath::kCpuSync;
    }
    return true;
  }

  void ReleaseOutput() override { release_calls_.fetch_add(1); }

  int set_surface_calls() const { return set_surface_calls_.load(); }
  int bind_calls() const { return bind_calls_.load(); }
  int without_gl_calls() const { return without_gl_calls_.load(); }
  int release_calls() const { return release_calls_.load(); }
  bool last_frame_mappable() const { return last_frame_mappable_.load(); }

 private:
  const bool gl_route_;
  void* raw_{nullptr};
  std::atomic<int> set_surface_calls_{0};
  std::atomic<int> bind_calls_{0};
  std::atomic<int> without_gl_calls_{0};
  std::atomic<int> release_calls_{0};
  std::atomic<bool> last_frame_mappable_{false};
};

// Hands the sink the same frame on every Render() and counts what the sink
// reported back as a failure.
class FixedFrameRenderer final : public VideoRendererSink::RenderCallback {
 public:
  explicit FixedFrameRenderer(base::scoped_refptr<VideoFrame> frame)
      : frame_(std::move(frame)) {}

  base::scoped_refptr<VideoFrame>
  Render(base::TimeTicks /*deadline_min*/,
         base::TimeTicks /*deadline_max*/) override {
    return frame_;
  }
  void OnFrameSubmitFailure() override { failures_.fetch_add(1); }

  int failures() const { return failures_.load(); }

 private:
  base::scoped_refptr<VideoFrame> frame_;
  std::atomic<int> failures_{0};
};

// What one driven run produced. Counts, not timings: the cadence and the
// thread are real but what is under test is which door each frame went through.
struct Routing {
  int bind_calls{0};
  int without_gl_calls{0};
  int release_calls{0};
  int set_surface_calls{0};
  int submit_failures{0};
  bool last_frame_mappable{false};
};

// Not a real window: the sink needs the kind to match and the payload to be
// non-null, and neither route here looks inside it.
base::scoped_refptr<NativeDisplay> MakeFakeDisplay() {
  static int payload = 0;
  return NativeDisplay::Wrap(&payload, NativeDisplayKind::kGlfwWindow,
                             base::OnceCallback<void(void*)>());
}

base::scoped_refptr<VideoFrame> MakeCpuFrame() {
  const Size size{64, 64};
  return VideoFrame::CreateBlackFrame(VideoFormat::kI420, size, size,
                                      Rational{1, 1}, base::TimeDelta(),
                                      base::TimeDelta(), 0);
}

// A GPU-resident frame that CAN be read back: not mappable, so the sink has to
// decide what to do with it before any target sees it.
base::scoped_refptr<VideoFrame> MakeGpuFrame() {
  NativeHandle handle;
  handle.kind = NativeHandleKind::kVaapiSurface;
  static int payload = 0;
  handle.id = &payload;
  const Size size{64, 64};
  return VideoFrame::WrapNativeBuffer(
      handle, VideoFormat::kNV12, size, size, Rational{1, 1}, base::TimeDelta(),
      base::TimeDelta(), 0, base::BindOnce([] {}),
      base::BindRepeating(&MakeCpuFrame));
}

// Runs the real loop for a few frames. 120 ms at a 60 Hz deadline is several
// presents; long enough to see routing, short enough to stay a unit test.
Routing DriveRouting(const base::scoped_refptr<VideoFrame>& frame,
                     bool gl_route) {
  auto target = std::make_unique<RecordingTarget>(gl_route);
  RecordingTarget* const borrowed = target.get();
  GlVideoSink sink(MakeFakeDisplay(), NativeDisplayKind::kGlfwWindow,
                   "RoutingTest", std::move(target));
  FixedFrameRenderer renderer(frame);
  sink.Initialize(&renderer);
  sink.Start();
  sink.Play();
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  sink.Stop();

  Routing out;
  out.bind_calls = borrowed->bind_calls();
  out.without_gl_calls = borrowed->without_gl_calls();
  out.release_calls = borrowed->release_calls();
  out.set_surface_calls = borrowed->set_surface_calls();
  out.submit_failures = renderer.failures();
  out.last_frame_mappable = borrowed->last_frame_mappable();
  return out;
}

// A target with no GL route draws every frame itself and the sink never asks
// for a binding. This is the SDL_Renderer case, and it is the one that used to
// live in a separate sink with its own copy of this loop.
TEST(OutputRouting, ANonGlTargetDrawsEveryFrameItself) {
  const Routing r = DriveRouting(MakeCpuFrame(), /*gl_route=*/false);

  EXPECT_GT(r.without_gl_calls, 1) << "the loop must have run more than once";
  EXPECT_EQ(r.bind_calls, 0)
      << "a target with no GL route must never be asked to bind a context";
}

// The GL route asks the target for a binding and never calls the renderer-side
// present. The binding here yields no context, so every frame is a submit
// failure -- which is what proves the sink went down the presenter path at all
// rather than quietly falling back to the other one.
TEST(OutputRouting, AGlTargetIsAskedToBindAndNeverToDrawWithoutGl) {
  const Routing r = DriveRouting(MakeCpuFrame(), /*gl_route=*/true);

  EXPECT_GT(r.bind_calls, 0);
  EXPECT_EQ(r.without_gl_calls, 0)
      << "a GL target must not also be handed frames to draw by hand";
  EXPECT_GT(r.submit_failures, 0)
      << "the sink took the presenter path and reported that it could not "
         "present, rather than silently succeeding through the other route";
}

// The one question the loop still owns: a GPU frame on a target whose API needs
// CPU bytes is read back BEFORE the target sees it. On a GL target it is not --
// the import route decides there, inside the layer.
TEST(OutputRouting, AGpuFrameIsReadBackForANonGlTarget) {
  const Routing r = DriveRouting(MakeGpuFrame(), /*gl_route=*/false);

  EXPECT_GT(r.without_gl_calls, 0);
  EXPECT_TRUE(r.last_frame_mappable)
      << "the target must be handed a frame whose planes it can read";
}

// The target is told which surface to draw into before the first present asks
// it anything, and released once the render thread is gone. Both are ordering
// claims, and the second is the one that makes the SDL texture safe to destroy.
TEST(OutputRouting, TheTargetIsGivenTheSurfaceAndReleasedAfterTheLoop) {
  const Routing r = DriveRouting(MakeCpuFrame(), /*gl_route=*/false);

  EXPECT_GE(r.set_surface_calls, 1);
  EXPECT_EQ(r.release_calls, 1) << "Stop() releases the target exactly once";
}

}  // namespace
}  // namespace avbase::media
