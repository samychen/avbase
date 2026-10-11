// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/null_video_sink.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"

namespace avbase::media {

NullVideoSink::NullVideoSink() = default;

NullVideoSink::~NullVideoSink() {
  // Stop() joins the render thread before any member is torn down, so no
  // in-flight RenderOne() can observe a half-destroyed sink.
  Stop();
}

void NullVideoSink::Initialize(RenderCallback* callback) {
  callback_ = callback;
}

void NullVideoSink::Start() {
  if (!thread_) {
    thread_ = std::make_unique<base::Thread>("avbase-null-video");
    thread_->Start();
    render_runner_ = thread_->task_runner();
  }
  RenderOne();
}

void NullVideoSink::Stop() {
  playing_.store(false);
  if (thread_) {
    thread_->Stop();
    thread_.reset();
    render_runner_ = nullptr;
  }
}

void NullVideoSink::Pause() {
  playing_.store(false);
}

void NullVideoSink::Play() {
  playing_.store(true);
}

void NullVideoSink::Flush() {
  // Nothing buffered here; the compositor owns the frames.
}

void NullVideoSink::SetOutputTarget(
    base::scoped_refptr<NativeDisplay> display) {
  // A null (or None) display means "render nowhere" -- which is what this sink
  // always does. Keeping the flag makes the stats honest: a discarded frame is
  // not a presented one.
  discard_.store(!display || !display->valid());
}

bool NullVideoSink::IsRunning() const {
  return thread_ != nullptr;
}

bool NullVideoSink::GetDisplayInterval(base::TimeDelta* interval) const {
  // No vsync source. Returning false is the documented way to tell the
  // compositor to drive cadence from container timestamps instead.
  (void)interval;
  return false;
}

VideoSinkStats NullVideoSink::GetStats() const {
  return stats_.Get();
}

void NullVideoSink::RenderOne() {
  const base::TimeTicks frame_start = base::TimeTicks::Now();
  // Render is called even in discard mode: "render nowhere" still means
  // "keep asking the compositor", which is what drives its frame pacing and
  // keeps the video clock alive for video-only streams (docs/07 §4,
  // SetOutputTargetNullEntersDiscardMode). The frame is then counted, not
  // shown -- a discarded frame is not a presented one.
  if (playing_.load() && callback_) {
    base::scoped_refptr<VideoFrame> frame =
        callback_->Render(base::TimeTicks(), base::TimeTicks());
    // There is no upload to fail and no hardware frame to read back, so the
    // only question is whether this frame was shown. Same classification the
    // sinks with a display use, so "presented" means the same thing here.
    const bool shown = frame != nullptr && !discard_.load();
    stats_.NoteFrameOutcome(
        ClassifyFrameOutcome(frame != nullptr, discard_.load(),
                             /*readback_failed=*/false, shown));
  }
  if (render_runner_) {
    // Re-arm from inside the task: one chained delayed task per tick, so Stop()
    // is simply "the next arm never happens plus Thread::Stop() joins".
    //
    // The delay comes from NextFrameDelay for the same reason it does in the
    // sinks with a display: a timer that budgets each interval from "now"
    // charges its own wake-up overshoot to every period, and there is nothing
    // here that could absorb it -- this sink has no swap to block on.
    const FramePacing pacing =
        NextFrameDelay(frame_start, base::TimeTicks::Now(),
                       next_frame_deadline_, kDefaultFrameInterval);
    next_frame_deadline_ = pacing.next_deadline;
    render_runner_->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&NullVideoSink::RenderOne, base::Unretained(this)),
        pacing.delay);
  }
}

std::unique_ptr<VideoRendererSink>
NullVideoSinkFactory::Create(base::scoped_refptr<NativeDisplay> display) {
  auto sink = std::make_unique<NullVideoSink>();
  sink->SetOutputTarget(std::move(display));
  return sink;
}

}  // namespace avbase::media
