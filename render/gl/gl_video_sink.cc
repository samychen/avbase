// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/gl/gl_video_sink.h"

#include <atomic>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "render/gl/gl_present.h"

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
  PresentOne();
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
  // context it lent us.
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
  return stats;
}

void GlVideoSink::PresentOne() {
  if (playing_.load() && callback_) {
    base::scoped_refptr<VideoFrame> frame =
        callback_->Render(base::TimeTicks(), base::TimeTicks());
    // Zero-copy contract (avbase §6.2): a hardware frame arrives GPU-resident
    // and the GL path cannot take it directly. The readback is EXPLICIT here
    // -- ToI420() -- so the GPU path stays the default and the cost shows up
    // in exactly one place (see Sdl2VideoSink for the full rationale).
    if (frame && !frame->IsMappable()) {
      base::scoped_refptr<VideoFrame> mapped = frame->ToI420();
      if (!mapped) {
        // No readback path installed: treat as a submit failure so the
        // renderer's stats tell the truth instead of dropping silently.
        submit_failures_.fetch_add(1);
        frame = nullptr;
      } else {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
          LOG(WARNING) << name_
                       << " sink read back a hardware frame via ToI420(); "
                          "expect this only on the CPU-fallback display path";
        }
        frame = std::move(mapped);
      }
    }
    if (discard_.load() || !frame) {
      frames_dropped_.fetch_add(1);
    } else if (UploadAndPresent(*frame)) {
      frames_presented_.fetch_add(1);
    } else {
      submit_failures_.fetch_add(1);
    }
  }
  if (render_runner_) {
    render_runner_->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&GlVideoSink::PresentOne, base::Unretained(this)),
        kMaxPresentInterval);
  }
}

bool GlVideoSink::UploadAndPresent(const VideoFrame& frame) {
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
    if (!presenter->Init()) {
      // Context exists but the shader path is unusable (e.g. a GL 2.0-era
      // driver): report failure rather than silently degrade the picture.
      LOG(ERROR) << name_ << " gl presenter init failed";
      return false;
    }
    presenter_ = std::move(presenter);
  }
  return presenter_->Present(frame);
}

}  // namespace avbase::media
