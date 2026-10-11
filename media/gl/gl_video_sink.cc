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
#include "media/gl/gl_present.h"

namespace avbase::media {
namespace {

// Render cadence when the sink has no vsync source to block on (Sdl2VideoSink
// uses the same bound). With a swap interval the present below blocks for the
// real refresh, so this only caps the *maximum* rate; without it, this is the
// rate.
constexpr base::TimeDelta kMaxPresentInterval = base::Milliseconds(1000) / 60;

}  // namespace

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
    *interval = kMaxPresentInterval;
  }
  // An estimate, not a measured vsync: returning true makes the compositor
  // use this as its display window, closer to the truth than "no
  // information" for the common 60 Hz case.
  return true;
}

VideoSinkStats GlVideoSink::GetStats() const {
  VideoSinkStats stats;
  stats.frames_presented = frames_presented_.load();
  stats.frames_dropped = frames_dropped_.load();
  stats.submit_failures = submit_failures_.load();
  base::AutoLock scoped(stats_lock_);
  stats.render = render_stats_;
  if (present_interval_ms_.samples() > 0 &&
      present_interval_ms_.average_ms() > 0.0) {
    stats.measured_refresh_hz = 1000.0 / present_interval_ms_.average_ms();
  }
  return stats;
}

void GlVideoSink::NotePresentTiming(const PresentTiming& timing) {
  const base::TimeTicks now = base::TimeTicks::Now();
  base::AutoLock scoped(stats_lock_);
  render_stats_.upload.Add(timing.upload_ms);
  render_stats_.compose.Add(timing.compose_ms);
  render_stats_.blit.Add(timing.blit_ms);
  render_stats_.present.Add(timing.present_ms);
  render_stats_.upload_path = timing.upload_path;
  // The spacing between presents is the only honest source for the display
  // refresh: GetDisplayInterval() above returns the 60 Hz estimate it was
  // constructed with, and this is what turns that estimate into a measurement.
  if (!last_present_ticks_.is_null() && now > last_present_ticks_) {
    present_interval_ms_.Add((now - last_present_ticks_).InMillisecondsF());
  }
  last_present_ticks_ = now;
}

bool GlVideoSink::CanImportHardwareFrame(const VideoFrame& frame) const {
  if (!hw_importer_ || hw_import_failed_.load()) {
    return false;
  }
  return hw_importer_->CanImport(frame);
}

base::scoped_refptr<VideoFrame> GlVideoSink::ReadBackHardwareFrame(
    const base::scoped_refptr<VideoFrame>& frame) {
  base::scoped_refptr<VideoFrame> mapped = frame->ToI420();
  if (!mapped) {
    return nullptr;
  }
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true)) {
    LOG(WARNING) << name_
                 << " sink read back a hardware frame via ToI420(); "
                    "expect this only on the CPU-fallback display path";
  }
  return mapped;
}

void GlVideoSink::PresentOne() {
  const base::TimeTicks frame_start = base::TimeTicks::Now();
  if (playing_.load() && callback_) {
    base::scoped_refptr<VideoFrame> frame =
        callback_->Render(base::TimeTicks(), base::TimeTicks());
    // Zero-copy contract (avbase §6.2): a hardware frame arrives GPU-resident,
    // and the sink's job is to keep it that way when it can. Which of the two
    // happens is a property of the HOST, not of the frame:
    //
    //   * a host with an import backend takes the frame as it is, and the
    //     presenter points its plane textures at the decoded surface;
    //   * otherwise the readback is EXPLICIT here -- ToI420() -- so the GPU
    //     path stays the default and the cost shows up in exactly one place.
    //
    // CanImportHardwareFrame() answers the first question, and it can answer
    // it before a presenter exists, which matters: on the first frame there
    // is no context current yet, so a question that needed one would be asked
    // at the one moment it cannot be answered.
    if (frame && !frame->IsMappable() && !CanImportHardwareFrame(*frame)) {
      frame = ReadBackHardwareFrame(frame);
      if (!frame) {
        // No readback path installed: treat as a submit failure so the
        // renderer's stats tell the truth instead of dropping silently.
        submit_failures_.fetch_add(1);
      }
    }
    if (discard_.load() || !frame) {
      frames_dropped_.fetch_add(1);
    } else if (UploadAndPresent(frame)) {
      frames_presented_.fetch_add(1);
    } else {
      submit_failures_.fetch_add(1);
    }
  }
  if (render_runner_) {
    // Budget the interval from the START of this frame, not from now. The
    // present above can already block on vsync inside SwapBuffers (that wait
    // is what present_ms measures), and scheduling a further full interval
    // would add the two: a 60 fps source was measured presenting at 30 fps for
    // exactly this reason.
    //
    // Two regimes, and picking between them is the whole point:
    //   * The swap blocked -- it already waited for the refresh, so it IS the
    //     throttle. Waiting again would double-count, so go straight back.
    //   * The swap returned at once (no vsync: offscreen, or a driver with the
    //     interval at 0) -- then the timer is the only thing pacing and it
    //     must supply the whole interval.
    // The threshold is half a refresh: a swap that took at least that long was
    // waiting for something, not doing work.
    const base::TimeDelta spent = base::TimeTicks::Now() - frame_start;
    const bool swap_waited = spent > kMaxPresentInterval / 2;
    base::TimeDelta delay;
    if (swap_waited || spent >= kMaxPresentInterval) {
      delay = base::TimeDelta();
    } else {
      delay = kMaxPresentInterval - spent;
    }
    render_runner_->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&GlVideoSink::PresentOne, base::Unretained(this)),
        delay);
  }
}

void GlVideoSink::SetSceneUpdate(GlSceneDriver::UpdateCallback callback) {
  scene_.SetUpdateCallback(std::move(callback));
}

void GlVideoSink::SetHwTextureImporter(
    std::unique_ptr<GlHwTextureImporter> importer) {
  hw_importer_ = std::move(importer);
  // A new backend deserves a fresh chance: the sticky flag belongs to the
  // driver that refused, and this is a different one.
  hw_import_failed_.store(false);
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
  // The import was refused at the last step -- the backend accepted the frame
  // but the driver would not point a texture at its surface. One readback
  // beats a black screen, and the import stays off afterwards instead of being
  // retried every frame, because a driver that refuses this API refuses it
  // forever. The frame is still alive here, which is what makes the retry
  // possible at all.
  if (hw_importer_ && !frame->IsMappable() &&
      !hw_import_failed_.exchange(true)) {
    LOG(ERROR) << name_
               << " hardware import failed at draw time; read back this frame "
                  "and every frame after it";
    base::scoped_refptr<VideoFrame> mapped = frame->ToI420();
    if (mapped) {
      PresentTiming retry;
      if (scene_.Present(*presenter_, mapped, &retry)) {
        NotePresentTiming(retry);
        return true;
      }
    }
  }
  return false;
}

}  // namespace avbase::media
