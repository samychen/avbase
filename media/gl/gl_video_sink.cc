// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_video_sink.h"

#include <atomic>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "media/base/render_cadence.h"
#include "media/gl/gl_present.h"

namespace avbase::media {

GlVideoSink::GlVideoSink(base::scoped_refptr<NativeDisplay> display,
                         NativeDisplayKind expected_kind, const char* name)
    : display_(std::move(display)), expected_kind_(expected_kind), name_(name) {
  discard_.store(!display_ || display_->kind() != expected_kind_ ||
                 !display_->raw());
}

GlVideoSink::~GlVideoSink() {
  Stop();
}

void GlVideoSink::Initialize(RenderCallback* callback) {
  callback_ = callback;
}

void GlVideoSink::Start() {
  if (thread_) {
    return;
  }
  // A restarted loop has no cadence to inherit: the deadline from the previous
  // run is a time that already passed.
  next_frame_deadline_ = base::TimeTicks();
  thread_ = std::make_unique<base::Thread>("avbase-gl-video");
  thread_->Start();
  render_runner_ = thread_->task_runner();
  // The first present must run on the render thread, not here:
  // UploadAndPresent() lazily binds the GL context, and a GL context is owned
  // by whoever made it current. Doing it synchronously would leave the context
  // current on Start()'s caller thread, and the render thread's later
  // MakeCurrent() would not take ownership -- every present then runs with no
  // current context and every shader silently fails to compile.
  render_runner_->PostTask(FROM_HERE, base::BindOnce(&GlVideoSink::PresentOne,
                                                     base::Unretained(this)));
}

void GlVideoSink::Stop() {
  playing_.store(false);
  if (thread_) {
    // Join the render thread before touching the presenter: after Stop()
    // returns, PresentOne() can no longer be in flight (the contract the
    // sub-renderer's teardown ordering relies on).
    thread_->Stop();
    thread_.reset();
    render_runner_ = nullptr;
  }
  // The presenter must die after the render thread is gone (its GL objects
  // belong to that thread's context) but before the host may destroy the
  // context it lent us. The scene driver goes first: it only drops a borrow,
  // and the presenter is what actually deletes the scene.
  scene_.Shutdown();
  presenter_.reset();
  overlay_slot_ = nullptr;
}

void GlVideoSink::Pause() {
  playing_.store(false);
}

void GlVideoSink::Play() {
  playing_.store(true);
}

void GlVideoSink::Flush() {
  // The compositor owns buffered frames; the texture keeps showing whatever
  // it showed, which is the right post-flush picture until the next present.
}

void GlVideoSink::SetOutputTarget(base::scoped_refptr<NativeDisplay> display) {
  display_ = std::move(display);
  discard_.store(!display_ || display_->kind() != expected_kind_ ||
                 !display_->raw());
  // Drop the presenter so the next present rebuilds it for the new context;
  // overlay_slot_ is re-fetched from BindSurface() together with the context.
  // The scene goes with it: its GL objects belong to the context being
  // abandoned, so keeping it would mean drawing through a dead context.
  scene_.Shutdown();
  presenter_.reset();
  overlay_slot_ = nullptr;
}

bool GlVideoSink::IsRunning() const {
  return thread_ != nullptr;
}

bool GlVideoSink::GetDisplayInterval(base::TimeDelta* interval) const {
  if (interval) {
    *interval = kDefaultFrameInterval;
  }
  // An estimate, not a measured vsync: returning true makes the compositor
  // use this as its display window, closer to the truth than "no
  // information" for the common 60 Hz case.
  return true;
}

VideoSinkStats GlVideoSink::GetStats() const {
  return stats_.Get();
}

void GlVideoSink::NotePresentTiming(const PresentTiming& timing) {
  stats_.NotePresentTiming(timing);
}

void GlVideoSink::PresentOne() {
  const base::TimeTicks frame_start = base::TimeTicks::Now();
  if (playing_.load() && callback_) {
    base::scoped_refptr<VideoFrame> frame =
        callback_->Render(base::TimeTicks(), base::TimeTicks());
    // Zero-copy contract (avbase §6.2): a hardware frame arrives GPU-resident,
    // and the sink's job is to keep it that way when it can. It does NOT decide
    // what happens when it cannot: whether a GPU frame is imported, read back,
    // or reported as a failure is GlVideoLayer::Upload()'s call, because that
    // is the one place that knows the frame is still alive and unused. Deciding
    // it here as well is how this sink grew a second, divergent copy of the
    // rule -- one before the present and one after a refused import.
    const bool presented = !discard_.load() && frame && UploadAndPresent(frame);
    const FrameOutcome outcome =
        ClassifyFrameOutcome(frame != nullptr, discard_.load(),
                             /*readback_failed=*/false, presented);
    if (outcome == FrameOutcome::kSubmitFailure) {
      // The renderer keeps its own rate-limited count of these; nothing called
      // it before, so a submit failure was invisible to the pipeline that
      // handed the frame over.
      callback_->OnFrameSubmitFailure();
    }
    stats_.NoteFrameOutcome(outcome);
  }
  if (render_runner_) {
    // The cadence lives in media/base rather than here because the SDL2 sink
    // needs the identical rule, and a rule written twice gets fixed once.
    // Which of the two regimes applies is decided per frame; see
    // NextFrameDelay.
    const FramePacing pacing =
        NextFrameDelay(frame_start, base::TimeTicks::Now(),
                       next_frame_deadline_, kDefaultFrameInterval);
    next_frame_deadline_ = pacing.next_deadline;
    render_runner_->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&GlVideoSink::PresentOne, base::Unretained(this)),
        pacing.delay);
  }
}

void GlVideoSink::SetSceneUpdate(GlSceneDriver::UpdateCallback callback) {
  scene_.SetUpdateCallback(std::move(callback));
}

void GlVideoSink::SetHwTextureImporter(
    std::unique_ptr<GlHwTextureImporter> importer) {
  hw_importer_ = std::move(importer);
  // One line per sink, so "which route is this host taking" is answerable from
  // a log rather than from a profiler. "none" is a normal answer, not a
  // failure: it is every host without an interop backend.
  LOG(INFO) << name_ << " hardware import backend: "
            << (hw_importer_ ? hw_importer_->name() : "none");
}

bool GlVideoSink::UploadAndPresent(
    const base::scoped_refptr<VideoFrame>& frame) {
  if (!presenter_) {
    // Lazily bind on the render thread: Init() makes the context current
    // here, which is why the host must never make it current itself.
    GlSurfaceBinding binding = BindSurface(display_->raw());
    overlay_slot_ = binding.overlay;
    if (!binding.context) {
      return false;
    }
    auto presenter = std::make_unique<GlPresenter>(std::move(binding.context));
    presenter->SetOverlaySource(overlay_slot_);
    // Before Init(), so the first composed frame already lands in the canvas
    // the host asked for rather than being reallocated on frame two.
    presenter->SetCanvasSize(binding.canvas_width, binding.canvas_height);
    presenter->SetHwTextureImporter(hw_importer_.get());
    if (!presenter->Init()) {
      // Context exists but the shader path is unusable (e.g. a GL 2.0-era
      // driver): report failure rather than silently degrade the picture.
      LOG(ERROR) << name_ << " gl presenter init failed";
      return false;
    }
    presenter_ = std::move(presenter);
  }
  PresentTiming timing;
  // One call either way: the driver forwards to Present() when the host
  // installed no scene hook, so the single-frame path is not a second branch
  // here that could drift from the scene one.
  if (scene_.Present(*presenter_, frame, &timing)) {
    NotePresentTiming(timing);
    return true;
  }
  // No second attempt here. A frame the import route would not take is one
  // Upload() has already read back and retried, so failing now means failing
  // twice -- and the retry used to live in BOTH places, which is why the two
  // sinks could disagree about whether a refused import was a dropped frame.
  return false;
}

}  // namespace avbase::media
