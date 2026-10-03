// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/null_video_sink.h"

#include "base/functional/bind.h"
#include "base/location.h"

namespace avbase::media {
namespace {

// Presentation cadence for a sink with no display. 60 Hz is the most common
// refresh rate and costs one cheap callback per tick; the compositor's own
// timestamp logic decides whether anything is actually due, so the exact
// number only trades a little idle CPU against presentation granularity.
constexpr base::TimeDelta kNullRenderInterval = base::Milliseconds(1000) / 60;

}  // namespace

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
  VideoSinkStats stats;
  stats.frames_presented = frames_presented_.load();
  stats.frames_dropped = frames_dropped_.load();
  stats.submit_failures = submit_failures_.load();
  return stats;
}

void NullVideoSink::RenderOne() {
  // Render is called even in discard mode: "render nowhere" still means
  // "keep asking the compositor", which is what drives its frame pacing and
  // keeps the video clock alive for video-only streams (docs/07 §4,
  // SetOutputTargetNullEntersDiscardMode). The frame is then counted, not
  // shown -- a discarded frame is not a presented one.
  if (playing_.load() && callback_) {
    base::scoped_refptr<VideoFrame> frame =
        callback_->Render(base::TimeTicks(), base::TimeTicks());
    if (discard_.load() || !frame) {
      frames_dropped_.fetch_add(1);
    } else {
      frames_presented_.fetch_add(1);
    }
  }
  if (render_runner_) {
    // Re-arm from inside the task: one chained delayed task per tick, so Stop()
    // is simply "the next arm never happens plus Thread::Stop() joins".
    render_runner_->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&NullVideoSink::RenderOne, base::Unretained(this)),
        kNullRenderInterval);
  }
}

std::unique_ptr<VideoRendererSink>
NullVideoSinkFactory::Create(base::scoped_refptr<NativeDisplay> display) {
  auto sink = std::make_unique<NullVideoSink>();
  sink->SetOutputTarget(std::move(display));
  return sink;
}

}  // namespace avbase::media
