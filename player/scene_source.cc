// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The half of the scene that a PRODUCER talks to: a SceneSource is a mailbox
// with a name and a draw order, and everything here is about getting frames
// into it and counting what happened to them. The half that draws -- Scene,
// SceneFrame and the per-frame handle -- is in scene.cc.

#include "player/public/scene.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#include "media/base/render_cadence.h"
#include "media/base/video_renderer_sink.h"
#include "media/gl/gl_layer_source.h"

namespace avbase {

// ---- SceneSource -----------------------------------------------------------

SceneSource::SceneSource(std::string name, int z)
    : name_(std::move(name)), z_(z) {}

void SceneSource::Bind(std::shared_ptr<media::GlLayerSource> source) {
  base::AutoLock scoped(lock_);
  RetireLocked();
  source_ = std::move(source);
}

void SceneSource::Retire() {
  base::AutoLock scoped(lock_);
  RetireLocked();
}

uint64_t SceneSource::frames_published() const {
  base::AutoLock scoped(lock_);
  uint64_t n = carried_published_;
  return source_ ? n + source_->frames_published() : n;
}

uint64_t SceneSource::frames_consumed() const {
  base::AutoLock scoped(lock_);
  uint64_t n = carried_consumed_;
  return source_ ? n + source_->frames_consumed() : n;
}

uint64_t SceneSource::frames_superseded() const {
  base::AutoLock scoped(lock_);
  uint64_t n = carried_superseded_;
  return source_ ? n + source_->frames_superseded() : n;
}

bool SceneSource::Publish(base::scoped_refptr<media::VideoFrame> frame) {
  base::AutoLock scoped(lock_);
  if (!source_) {
    return false;
  }
  source_->Publish(std::move(frame));
  return true;
}

// ---- SceneSource::MakePublisherFactory --------------------------------------
namespace {

// The second stream's endpoint: it draws nothing, and every frame the
// compositor hands it is published into a SceneSource, which the owning
// player's sink drains once per displayed frame.
//
// THE MAILBOX IS THE POINT, not the poll loop. Two pipelines running at their
// own rates would otherwise have to be made to agree on one; here they never
// meet, and the display's clock is the only one that decides what is shown.
class PublisherSink final : public media::VideoRendererSink {
 public:
  explicit PublisherSink(std::shared_ptr<SceneSource> source)
      : source_(std::move(source)) {}
  PublisherSink(const PublisherSink&) = delete;
  PublisherSink& operator=(const PublisherSink&) = delete;
  ~PublisherSink() override { Stop(); }

  void Initialize(RenderCallback* callback) override { upstream_ = callback; }

  void Start() override {
    if (worker_.joinable()) {
      return;
    }
    running_.store(true);
    playing_.store(true);
    worker_ = std::thread([this] { PollLoop(); });
  }

  void Stop() override {
    running_.store(false);
    if (worker_.joinable()) {
      worker_.join();
    }
    // Deliberately no Detach(): whatever this source was holding is released
    // by the scene when it unbinds, and reaching past SceneSource to do it by
    // hand is exactly the coupling this class exists to remove.
  }

  void Pause() override { playing_.store(false); }
  void Play() override { playing_.store(true); }
  void Flush() override {}
  void SetOutputTarget(
      base::scoped_refptr<media::NativeDisplay> /*display*/) override {
    // No window to move to: the picture goes into the mailbox either way.
  }

  bool IsRunning() const override { return running_.load(); }
  bool GetDisplayInterval(base::TimeDelta* interval) const override {
    if (interval) {
      *interval = media::kDefaultFrameInterval;
    }
    return true;
  }

  media::VideoSinkStats GetStats() const override {
    media::VideoSinkStats stats;
    stats.frames_presented = source_ ? source_->frames_consumed() : 0;
    return stats;
  }

  const char* name() const override { return "publisher"; }

 private:
  // The pull side of the second player's compositor: one frame per tick, which
  // is all a mailbox that keeps only the newest one can make use of.
  void PollLoop() {
    base::TimeTicks deadline;
    while (running_.load()) {
      const base::TimeTicks frame_start = base::TimeTicks::Now();
      if (playing_.load() && upstream_ != nullptr) {
        base::scoped_refptr<media::VideoFrame> frame =
            upstream_->Render(base::TimeTicks(), base::TimeTicks());
        if (frame) {
          source_->Publish(std::move(frame));
        }
      }
      const media::FramePacing pacing =
          media::NextFrameDelay(frame_start, base::TimeTicks::Now(), deadline,
                                media::kDefaultFrameInterval);
      deadline = pacing.next_deadline;
      std::this_thread::sleep_for(
          std::chrono::microseconds(pacing.delay.InMicroseconds()));
    }
  }

  std::shared_ptr<SceneSource> source_;
  RenderCallback* upstream_{nullptr};
  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> playing_{false};
};

class ScenePublisherSinkFactory final : public media::VideoRendererSinkFactory {
 public:
  explicit ScenePublisherSinkFactory(std::shared_ptr<SceneSource> source)
      : source_(std::move(source)) {}

  std::unique_ptr<media::VideoRendererSink>
  Create(base::scoped_refptr<media::NativeDisplay> /*display*/) override {
    return std::make_unique<PublisherSink>(source_);
  }
  const char* name() const override { return "scene-publisher"; }

 private:
  std::shared_ptr<SceneSource> source_;
};

}  // namespace

std::shared_ptr<media::VideoRendererSinkFactory>
SceneSource::MakePublisherFactory() {
  return std::make_shared<ScenePublisherSinkFactory>(shared_from_this());
}

// Declared out of line so none of this has to appear in a public header.
void SceneSource::RetireLocked() {
  if (!source_) {
    return;
  }
  carried_published_ += source_->frames_published();
  carried_consumed_ += source_->frames_consumed();
  carried_superseded_ += source_->frames_superseded();
  source_.reset();
}

}  // namespace avbase
