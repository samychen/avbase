// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_GL_GL_OUTPUT_TARGET_H_
#define AVBASE_MEDIA_GL_GL_OUTPUT_TARGET_H_

#include <memory>

#include "media/base/render_perf_stats.h"
#include "media/base/video_frame.h"
#include "media/media_export.h"
#include "render/gl/native_gl_context.h"
#include "render/gl/overlay.h"

namespace avbase::media {

// The pairing a target hands the sink for one present: the GL context to
// present through, plus the optional overlay slot to composite over it, plus
// the optional composition size to compose at.
//
// The canvas travels with the binding rather than through a setter on the sink
// because it is a property of the SURFACE being drawn into: a host that knows
// it wants 1080p composition states it where it states the window, and a
// target that has no opinion leaves both at 0 ("follow the drawable").
struct AVBASE_MEDIA_EXPORT GlSurfaceBinding {
  std::unique_ptr<gl::NativeGLContext> context;
  gl::TextOverlaySlot* overlay{nullptr};
  // Composition surface in pixels; non-positive in either axis means the
  // composition follows the drawable. See GlPresenter::SetCanvasSize().
  int canvas_width{0};
  int canvas_height{0};
};

// One way of getting a frame onto one display.
//
// This is the whole of what differs between backends, and nothing that does
// not. The render thread, the cadence, the stats, the scene driver and the
// hardware importer are the same for every target and live in GlVideoSink; a
// target answers only "what am I drawing into, and through which API".
//
// WHY A COMPOSED OBJECT AND NOT A SECOND SINK. Sdl2VideoSink used to carry a
// byte-identical copy of the render loop -- Start/Stop/Pause/Play/Flush/
// SetOutputTarget/PresentOne -- because it could be drawing through either a
// GL context or an SDL_Renderer and nothing else knew which. Two sinks with
// two copies of a cadence rule is two places to fix one bug, which is exactly
// why NextFrameDelay() and PresentStats were pulled out of them earlier; the
// loop itself was the last thing still duplicated, and it is the part with the
// teardown ordering in it.
//
// THREADING: SetSurface() and ReleaseOutput() run on whichever thread called
// Start()/SetOutputTarget()/Stop(); BindSurface() and PresentWithoutGl() run on
// the sink's render thread. ReleaseOutput() is called with the render thread
// already joined, so nothing of the target's is reachable concurrently.
class AVBASE_MEDIA_EXPORT GlOutputTarget {
 public:
  GlOutputTarget() = default;
  GlOutputTarget(const GlOutputTarget&) = delete;
  GlOutputTarget& operator=(const GlOutputTarget&) = delete;
  virtual ~GlOutputTarget() = default;

  // The display's opaque surface pointer changed -- including to null, which
  // is what a Stop() or a detach looks like.
  virtual void SetSurface(void* raw) = 0;

  // Whether this target presents through GL at all. A sink-shaped question
  // rather than a target-shaped one, but the target is the only thing that
  // knows: it is the answer that decides whether a GPU frame is read back
  // before the present, and getting it wrong hands a GPU frame to an API that
  // needs CPU bytes.
  virtual bool has_gl_route() const = 0;

  // The GL route: what a GlPresenter needs in order to draw into this target.
  // Only called when has_gl_route() is true.
  virtual GlSurfaceBinding BindSurface() = 0;

  // The route for a target with no GL: draws |frame| itself and fills |timing|
  // with the same three stages the GL path reports, so the two stay
  // comparable. Only called when has_gl_route() is false.
  virtual bool PresentWithoutGl(const VideoFrame& frame,
                                PresentTiming* timing) = 0;

  // Drops everything this target owns (textures, cached pointers). Called from
  // Stop() once the render thread is gone.
  virtual void ReleaseOutput() = 0;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_OUTPUT_TARGET_H_
