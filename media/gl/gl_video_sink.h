// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_GL_GL_VIDEO_SINK_H_
#define AVBASE_MEDIA_GL_GL_VIDEO_SINK_H_

#include <atomic>
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "media/base/native_display.h"
#include "media/base/present_stats.h"
#include "media/base/render_perf_stats.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/gl/gl_hw_texture_importer.h"
#include "media/gl/gl_output_target.h"
#include "media/gl/gl_scene_engine.h"
#include "media/media_export.h"
#include "render/gl/overlay.h"

namespace avbase::media {

class GlPresenter;

// A window-system-neutral GL video sink. Every backend that presents through
// GlPresenter (GLFW, EGL, SDL2-GL, ...) drives the same render-thread cadence
// and stats here; the only backend-specific knowledge -- which surface to draw
// into, and through which API -- is expressed by the GlOutputTarget it is
// handed at construction.
//
// There used to be two sinks. Sdl2VideoSink carried a byte-identical copy of
// this loop because it could be drawing through either a GL context or an
// SDL_Renderer and nothing else knew which; the duplication was removed by
// moving that one question into a target object rather than by giving the sink
// a second implementation. A backend is now a GlOutputTarget, and a named sink
// subclass -- where one still exists -- is ten lines that install one.
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
  // |output| is how this sink reaches a display; it is owned here and outlives
  // every presenter, because a target outlives the context it lent out.
  GlVideoSink(base::scoped_refptr<NativeDisplay> display,
              NativeDisplayKind expected_kind, const char* name,
              std::unique_ptr<GlOutputTarget> output);
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
  // frame the stream delivered. See GlSceneEngine.
  //
  // Set it BEFORE Start(), like VideoRendererSink::Initialize(): the render
  // thread reads it without a lock, and this sink has no way to interrupt a
  // present that is already in flight.
  void SetSceneUpdate(GlSceneEngine::UpdateCallback callback);

  // Adds a second (third, ...) source to the composition and returns what its
  // producer publishes into. The engine owns it; the handle stays valid as
  // long as this sink or the caller holds it. See GlSceneEngine::AddSource --
  // in particular that the layer is created on the GL thread, so a host that
  // wants to place it does that with an update callback, not here.
  //
  // This is the whole of what a host has to do to composite a stream avbase is
  // not playing: decode it however it likes, Publish() the frames, and the
  // sink picks them up at the next present.
  std::shared_ptr<GlLayerSource> AddSceneSource(std::string name, int z);

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
  // The target this sink presents through. Borrowed by subclasses only so a
  // backend can ask it the one question the loop does not (SDL2's renderer
  // path uses it to reach the overlay); the sink owns it.
  GlOutputTarget* output() const { return output_.get(); }

 private:
  void PresentOne();
  bool UploadAndPresent(const base::scoped_refptr<VideoFrame>& frame);
  // Folds one present's stage timings into the published snapshot. Runs on the
  // render thread; the lock is a leaf (nothing is called while holding it), so
  // it cannot participate in a lock-order inversion.
  void NotePresentTiming(const PresentTiming& timing);

  base::scoped_refptr<NativeDisplay> display_;
  NativeDisplayKind expected_kind_;
  const char* name_;
  // Declared before presenter_ and engine_: those hold GL objects borrowed from
  // a context the target handed over, so the target has to outlive them.
  std::unique_ptr<GlOutputTarget> output_;
  base::raw_ptr<RenderCallback> callback_{nullptr};
  std::unique_ptr<base::Thread> thread_;
  base::scoped_refptr<base::SequencedTaskRunner> render_runner_;

  // Declared before presenter_, which borrows it: members die in reverse, so
  // the presenter is destroyed first and never sees a dangling importer.
  // Spans presenter rebuilds on purpose -- the interop API is a property of
  // the host, not of a context, so SetOutputTarget() has no reason to rebuild
  // it.
  std::unique_ptr<GlHwTextureImporter> hw_importer_;

  // Owns the GL objects (they belong to the render thread's context).
  std::unique_ptr<GlPresenter> presenter_;
  gl::TextOverlaySlot* overlay_slot_{nullptr};
  // Orchestrates the scene: the sink's own frame plus every source the host
  // added. Declared after presenter_ so it dies first -- the scene it borrows
  // holds GL objects from that context.
  GlSceneEngine engine_;

  // Cross-thread flags: written from any thread (Pause/Play/SetOutputTarget
  // are callable anywhere), read on the render thread.
  std::atomic<bool> playing_{false};
  std::atomic<bool> discard_{false};
  // Every number this sink reports, in one object: see media/base/
  // present_stats.h for why it is not spelled out here -- the SDL2 sink
  // carried a byte-identical copy of all of it.
  PresentStats stats_;
  // The cadence anchor for NextFrameDelay(): the deadline the loop is AIMING
  // at, which is not the time it last woke up. Null whenever the refresh is
  // doing the pacing rather than the timer.
  base::TimeTicks next_frame_deadline_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_VIDEO_SINK_H_
