// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/qt/qt_video_sink.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/threading/thread.h"
#include "media/base/video_sink_gate.h"

namespace avbase::qt {

// 60 Hz. This is a poll interval, not a promise: the compositor inside avbase
// decides which frame is current, and Render() returns the same frame on a
// refresh where none is due. Storing it again is cheap (one refcount bump) and
// harmless -- the mailbox serial advances and the GUI repaints.
namespace {

constexpr base::TimeDelta kPollInterval = base::Milliseconds(16);

}  // namespace

// The poll loop's whole world. No pointer back to the sink, on purpose: the
// sink may be destroyed while a poll is still queued, and a queued poll must
// then be harmless rather than a use-after-free.
struct QtVideoSink::Context {
  base::scoped_refptr<base::SequencedTaskRunner> runner;
  std::atomic<bool> running{true};
  // Whether this tick should ask for a frame: playing, or one pull owed after
  // a flush. See media/base/video_sink_gate.h -- the same rule the three
  // in-tree sinks use, so this one cannot drift from them.
  media::VideoSinkGate gate;
  // Written by Stop() (any thread) while the loop reads it, hence atomic.
  std::atomic<RenderCallback*> upstream{nullptr};
  FrameMailbox* mailbox{nullptr};
  std::atomic<uint64_t> presented{0};
  // Render() must never be entered twice at once. avbase's compositor hands
  // out frames from a deque it pops on the calling thread, so two pollers are
  // two threads mutating that deque -- which showed up as a malloc_zone_error
  // inside VideoFrameCompositor::Render, not as anything resembling a race.
  std::atomic<bool> rendering{false};
};

QtVideoSink::QtVideoSink(FrameMailbox* mailbox) : mailbox_(mailbox) {}

QtVideoSink::~QtVideoSink() {
  Stop();
}

void QtVideoSink::Initialize(RenderCallback* callback) {
  upstream_ = callback;
}

void QtVideoSink::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!worker_) {
    worker_ = std::make_unique<base::Thread>("qt-video-mailbox");
    worker_->Start();
  }
  if (!context_) {
    context_ = std::make_shared<Context>();
    context_->runner = worker_->task_runner();
    context_->mailbox = mailbox_;
  }
  context_->running.store(true);
  context_->gate.SetPlaying(true);
  context_->upstream.store(upstream_);

  // The shared_ptr is captured BY VALUE: the queued task owns a reference, so
  // the context outlives both this call and, if it comes to that, the sink.
  context_->runner->PostDelayedTask(
      FROM_HERE, base::BindOnce(&QtVideoSink::PollOnce, context_),
      base::TimeDelta());
}

void QtVideoSink::Stop() {
  // The join happens UNDER the lock, deliberately. PollOnce no longer takes
  // this lock (it only touches its own Context), so there is no deadlock --
  // and holding it is what stops a Start() racing in and creating a second
  // poll loop while the first one is still inside Render(). That overlap is
  // exactly what corrupted the compositor's frame deque.
  std::lock_guard<std::mutex> lock(mutex_);
  if (context_) {
    // Order matters: stop the loop from re-arming, and drop the callback
    // pointer, BEFORE joining. The renderer tears its callback down around
    // this call, so a poll that ran after that would call through a dead
    // pointer.
    context_->running.store(false);
    context_->upstream.store(nullptr);
  }
  std::unique_ptr<base::Thread> worker = std::move(worker_);
  context_.reset();
  upstream_ = nullptr;
  if (worker) {
    worker->Stop();
  }
}

void QtVideoSink::Pause() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (context_) {
    context_->gate.SetPlaying(false);
  }
}

void QtVideoSink::Play() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (context_) {
    context_->gate.SetPlaying(true);
  }
}

void QtVideoSink::Flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (mailbox_ != nullptr) {
    mailbox_->Clear();
  }
  // The mail box dropped its frame and the compositor dropped its queue, so
  // the next frame that arrives is a different picture. Ask for it even if the
  // player is paused -- that is what makes a seek show its target.
  if (context_) {
    context_->gate.NoteFlushed();
  }
}

void QtVideoSink::SetOutputTarget(
    base::scoped_refptr<media::NativeDisplay> /*display*/) {
  // Deliberately empty: this sink presents by handing frames to the GUI
  // thread, not by drawing into a window, so there is no target to bind. The
  // host still calls SetVideoSurface(NativeDisplay::None()) for the API's own
  // bookkeeping -- "render nowhere, keep audio" is exactly what this is.
}

bool QtVideoSink::IsRunning() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return context_ != nullptr && context_->running.load();
}

bool QtVideoSink::GetDisplayInterval(base::TimeDelta* interval) const {
  if (interval != nullptr) {
    *interval = kPollInterval;
  }
  return true;
}

media::VideoSinkStats QtVideoSink::GetStats() const {
  media::VideoSinkStats stats;
  std::lock_guard<std::mutex> lock(mutex_);
  if (context_) {
    stats.frames_presented = context_->presented.load();
  }
  stats.measured_refresh_hz = 1000.0 / kPollInterval.InMillisecondsF();
  return stats;
}

void QtVideoSink::PollOnce(std::shared_ptr<Context> context) {
  if (!context || !context->running.load()) {
    return;
  }
  RenderCallback* upstream = context->upstream.load();
  if (context->gate.ShouldPull() && upstream != nullptr &&
      context->mailbox != nullptr) {
    // Belt and braces against two pollers: if a previous tick is somehow still
    // inside Render(), skip this one. Dropping a frame is always better than
    // racing the compositor's deque.
    bool idle = false;
    if (context->rendering.compare_exchange_strong(idle, true)) {
      auto frame = upstream->Render(base::TimeTicks(), base::TimeTicks());
      context->rendering.store(false);
      if (frame) {
        // Spends a flush's one-shot ask. A null frame ("keep showing the
        // previous one") spends nothing -- there is no new picture yet.
        context->gate.NoteFrameReceived();
        context->mailbox->Store(std::move(frame));
        context->presented.fetch_add(1);
      }
    }
  }
  if (context->running.load() && context->runner) {
    context->runner->PostDelayedTask(
        FROM_HERE, base::BindOnce(&QtVideoSink::PollOnce, std::move(context)),
        kPollInterval);
  }
}

std::unique_ptr<media::VideoRendererSink> QtVideoSinkFactory::Create(
    base::scoped_refptr<media::NativeDisplay> /*display*/) {
  return std::make_unique<QtVideoSink>(mailbox_);
}

}  // namespace avbase::qt
