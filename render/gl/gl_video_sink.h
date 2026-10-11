// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_GL_VIDEO_SINK_H_
#define AVBASE_RENDER_GL_GL_VIDEO_SINK_H_

#include <atomic>
#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "media/base/native_display.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "render/gl/native_gl_context.h"
#include "render/gl/overlay.h"

namespace avbase::media {

class GlPresenter;

// The pairing a backend hands the shared sink for one present: the GL context
// to present through, plus the optional overlay slot to composite over it.
struct AVBASE_MEDIA_EXPORT GlSurfaceBinding {
  std::unique_ptr<NativeGLContext> context;
  TextOverlaySlot* overlay{nullptr};
};

// A window-system-neutral GL video sink. Every backend that presents through
// GlPresenter (GLFW, EGL, SDL2-GL, ...) drives the same render-thread cadence
// and stats here; the only backend-specific knowledge -- how to turn the
// opaque surface pointer into a NativeGLContext -- is expressed by the one
// BindSurface() hook a subclass supplies.
//
// This is the factored-out GL half of Sdl2VideoSink: that class keeps its
// SDL_Renderer 2D path and delegates the GL path to the same GlPresenter this
// sink owns outright. Backends that are GL-only (no 2D API: GLFW, EGL)
// subclass this directly instead of re-deriving the whole render loop.
//
// THREADING: the host owns the window and its context (embedding mode,
// docs/archive/09 §2). This sink makes the context current only on its own
// render thread via the NativeGLContext it is given, so the host must never
// make the context current once it has handed the display to avbase.
class AVBASE_MEDIA_EXPORT GlVideoSink : public VideoRendererSink {
 public:
  GlVideoSink(base::scoped_refptr<NativeDisplay> display,
              NativeDisplayKind expected_kind, const char* name);
  GlVideoSink(const GlVideoSink&) = delete;
  GlVideoSink& operator=(const GlVideoSink&) = delete;
  ~GlVideoSink() override;

  // VideoRendererSink:
  void Initialize(RenderCallback* callback) override;
  void Start() override;
  void Stop() override;
  void Pause() override;
  void Play() override;
  void Flush() override;
  void SetOutputTarget(base::scoped_refptr<NativeDisplay> display) override;
  bool IsRunning() const override;
  bool GetDisplayInterval(base::TimeDelta* interval) const override;
  VideoSinkStats GetStats() const override;
  const char* name() const override { return name_; }

 protected:
  // Backend hook: given the opaque surface pointer carried in the display,
  // build the NativeGLContext the presenter needs (and report the overlay
  // slot). A null context puts the sink into present-failure for that frame.
  // Called on the render thread, once per (re)built presenter.
  virtual GlSurfaceBinding BindSurface(void* raw) = 0;

 private:
  void PresentOne();
  bool UploadAndPresent(const VideoFrame& frame);

  base::scoped_refptr<NativeDisplay> display_;
  NativeDisplayKind expected_kind_;
  const char* name_;
  base::raw_ptr<RenderCallback> callback_{nullptr};
  std::unique_ptr<base::Thread> thread_;
  base::scoped_refptr<base::SequencedTaskRunner> render_runner_;

  // Owns the GL objects (they belong to the render thread's context).
  std::unique_ptr<GlPresenter> presenter_;
  TextOverlaySlot* overlay_slot_{nullptr};

  // Cross-thread flags: written from any thread (Pause/Play/SetOutputTarget
  // are callable anywhere), read on the render thread.
  std::atomic<bool> playing_{false};
  std::atomic<bool> discard_{false};
  std::atomic<uint64_t> frames_presented_{0};
  std::atomic<uint64_t> frames_dropped_{0};
  std::atomic<uint64_t> submit_failures_{0};
};

}  // namespace avbase::media

#endif  // AVBASE_RENDER_GL_GL_VIDEO_SINK_H_
