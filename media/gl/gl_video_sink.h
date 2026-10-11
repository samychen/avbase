// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_GL_GL_VIDEO_SINK_H_
#define AVBASE_MEDIA_GL_GL_VIDEO_SINK_H_

#include <atomic>
#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "media/base/native_display.h"
#include "media/base/render_perf_stats.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/gl/gl_hw_texture_importer.h"
#include "media/gl/gl_scene_driver.h"
#include "media/media_export.h"
#include "render/gl/native_gl_context.h"
#include "render/gl/overlay.h"

namespace avbase::media {

class GlPresenter;

// The pairing a backend hands the shared sink for one present: the GL context
// to present through, plus the optional overlay slot to composite over it, plus
// the optional composition size to compose at.
//
// The canvas travels with the binding rather than through a setter on the sink
// because it is a property of the SURFACE being drawn into: a host that knows
// it wants 1080p composition states it where it states the window, and a
// backend that has no opinion leaves both at 0 ("follow the drawable").
struct AVBASE_MEDIA_EXPORT GlSurfaceBinding {
  std::unique_ptr<gl::NativeGLContext> context;
  gl::TextOverlaySlot* overlay{nullptr};
  // Composition surface in pixels; non-positive in either axis means the
  // composition follows the drawable. See GlPresenter::SetCanvasSize().
  int canvas_width{0};
  int canvas_height{0};
};

// A window-system-neutral GL video sink. Every backend that presents through
// GlPresenter (GLFW, EGL, SDL2-GL, ...) drives the same render-thread cadence
// and stats here; the only backend-specific knowledge -- how to turn the
// opaque surface pointer into a gl::NativeGLContext -- is expressed by the one
// BindSurface() hook a subclass supplies.
//
// This is the factored-out GL half of Sdl2VideoSink: that class keeps its
// SDL_Renderer 2D path and delegates the GL path to the same GlPresenter this
// sink owns outright. Backends that are GL-only (no 2D API: GLFW, EGL)
// subclass this directly instead of re-deriving the whole render loop.
//
// Lives under media/ rather than in the avbase_gl module because it is shaped
// by VideoRendererSink and VideoFrame: it is the video pipeline's render
// thread, not a reusable GL primitive.
//
// THREADING: the host owns the window and its context (embedding mode,
// docs/archive/09 §2). This sink makes the context current only on its own
// render thread via the gl::NativeGLContext it is given, so the host must not
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

  // Optional multi-source composition: installs a hook that is handed the
  // scene once per present, so the host can add or update layers around the
  // frame the stream delivered. See GlSceneDriver.
  //
  // Set it BEFORE Start(), like VideoRendererSink::Initialize(): the render
  // thread reads it without a lock, and this sink has no way to interrupt a
  // present that is already in flight.
  void SetSceneUpdate(GlSceneDriver::UpdateCallback callback);

  // Installs the host's hardware-import backend (platform/hwimport), and the
  // whole of what a backend has to do to get zero-copy playback: a null
  // pointer is legal and means "this host cannot", which is the state every
  // host without an interop backend is in.
  //
  // Set it BEFORE Start(), like SetSceneUpdate(): the render thread reads it
  // without a lock. The sink owns it and keeps it alive; the presenter only
  // borrows, and outliving a presenter rebuild is exactly why the sink holds
  // it rather than the presenter.
  void SetHwTextureImporter(std::unique_ptr<GlHwTextureImporter> importer);

 protected:
  // Backend hook: given the opaque surface pointer carried in the display,
  // build the gl::NativeGLContext the presenter needs (and report the overlay
  // slot). A null context puts the sink into present-failure for that frame.
  // Called on the render thread, once per (re)built presenter.
  virtual GlSurfaceBinding BindSurface(void* raw) = 0;

 private:
  void PresentOne();
  bool UploadAndPresent(const base::scoped_refptr<VideoFrame>& frame);
  // True when |frame| will be handed to the presenter as-is rather than read
  // back first. False without an importer, after an import has been refused,
  // or for a frame the backend declines -- all three of which mean the same
  // thing to the caller: this frame has to travel through CPU memory.
  bool CanImportHardwareFrame(const VideoFrame& frame) const;
  // ToI420() plus the once-per-sink warning. Null when the producer installed
  // no readback path, which the caller reports as a submit failure rather than
  // dropping the frame silently.
  base::scoped_refptr<VideoFrame>
  ReadBackHardwareFrame(const base::scoped_refptr<VideoFrame>& frame);
  // Folds one present's stage timings into the published snapshot. Runs on the
  // render thread; the lock is a leaf (nothing is called while holding it), so
  // it cannot participate in a lock-order inversion.
  void NotePresentTiming(const PresentTiming& timing);

  base::scoped_refptr<NativeDisplay> display_;
  NativeDisplayKind expected_kind_;
  const char* name_;
  base::raw_ptr<RenderCallback> callback_{nullptr};
  std::unique_ptr<base::Thread> thread_;
  base::scoped_refptr<base::SequencedTaskRunner> render_runner_;

  // Declared before presenter_, which borrows it: members die in reverse, so
  // the presenter is destroyed first and never sees a dangling importer.
  // Spans presenter rebuilds on purpose -- the interop API is a property of
  // the host, not of a context, so SetOutputTarget() has no reason to rebuild
  // it.
  std::unique_ptr<GlHwTextureImporter> hw_importer_;
  // Sticky, and the reason a refused import costs one frame instead of every
  // frame: a driver that refuses CGLTexImageIOSurface2D refuses it forever, so
  // the second attempt would only spend the same failure again.
  std::atomic<bool> hw_import_failed_{false};

  // Owns the GL objects (they belong to the render thread's context).
  std::unique_ptr<GlPresenter> presenter_;
  gl::TextOverlaySlot* overlay_slot_{nullptr};
  // Keeps a scene in step with the frames when the host installed a hook;
  // unused (and allocates nothing) otherwise. Declared after presenter_ so it
  // dies first -- its scene holds GL objects from that context.
  GlSceneDriver scene_;

  // Cross-thread flags: written from any thread (Pause/Play/SetOutputTarget
  // are callable anywhere), read on the render thread.
  std::atomic<bool> playing_{false};
  std::atomic<bool> discard_{false};
  std::atomic<uint64_t> frames_presented_{0};
  std::atomic<uint64_t> frames_dropped_{0};
  std::atomic<uint64_t> submit_failures_{0};

  // Render-thread telemetry, guarded because GetStats() reads it from the
  // media sequence (the one-second statistics tick). Written only on the
  // render thread; the copy taken under the lock is a handful of doubles, and
  // taking it keeps the stats a consistent snapshot instead of letting a
  // reader see a half-updated average.
  mutable base::Lock stats_lock_;
  RenderPerfStats render_stats_;
  // Wall-clock spacing between presents, for measured_refresh_hz -- the field
  // VideoSinkStats has always declared and nothing has ever filled.
  DurationEwma present_interval_ms_;
  base::TimeTicks last_present_ticks_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_VIDEO_SINK_H_
