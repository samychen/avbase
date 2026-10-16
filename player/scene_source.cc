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
#include "media/filters/null_video_sink.h"
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

// ---- In-pipeline secondary sink -------------------------------------------
// docs/14 stage 3 retired SceneSource::MakePublisherFactory() -- the public
// "second Player" path -- and the player now builds its secondary legs
// directly. The PublisherSink is KEPT as the in-pipeline adapter: each
// secondary leg's VideoRendererImpl hands it frames, and it publishes them into
// that leg's SceneSource, which the scene has already plumbed into the host
// sink's engine. So a second picture is now a second demuxer in the SAME
// pipeline, not a second Player -- same composited result, no mailbox-crossing
// clock mismatch.
namespace {

// The secondary stream's endpoint: it draws nothing, and every frame the
// compositor hands it is published into a SceneSource, which the host sink's
// engine drains once per displayed frame.
//
// THE MAILBOX IS THE POINT, not the poll loop. Two decoders running at their
// own rates would otherwise have to be made to agree on one; here they never
// meet -- the secondary leg is pulled on its own cadence and the display's
// clock (the host sink) is the only one that decides what is shown, because it
// is the one that composites.
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
  // The pull side of a secondary leg's compositor: one frame per tick, which
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

// Hands each secondary leg (source id >= 1) a PublisherSink into its own
// SceneSource. Indexed by source id: sources_[k] serves the leg whose source
// id is k + 1, because source 0 takes the dedicated host sink first and never
// reaches here. A leg whose id is out of range -- a mis-registration, which
// should not happen -- gets a NullVideoSink so it degrades rather than
// crashes.
class SceneSecondarySinkFactory final : public media::VideoRendererSinkFactory {
 public:
  explicit SceneSecondarySinkFactory(
      std::vector<std::shared_ptr<SceneSource>> sources)
      : sources_(std::move(sources)) {}

  std::unique_ptr<media::VideoRendererSink>
  Create(base::scoped_refptr<media::NativeDisplay> /*display*/) override {
    // A leg that somehow takes the non-source-aware path gets a drop sink: the
    // secondary legs always arrive through CreateForSource(), so reaching here
    // means a wiring mistake upstream, not a real configuration.
    return std::make_unique<media::NullVideoSink>();
  }

  std::unique_ptr<media::VideoRendererSink> CreateForSource(
      int source_id,
      base::scoped_refptr<media::NativeDisplay> /*display*/) override {
    const size_t index = static_cast<size_t>(source_id) - 1;
    if (source_id < 1 || index >= sources_.size() || !sources_[index]) {
      return std::make_unique<media::NullVideoSink>();
    }
    return std::make_unique<PublisherSink>(sources_[index]);
  }

  const char* name() const override { return "scene-secondary"; }

 private:
  std::vector<std::shared_ptr<SceneSource>> sources_;
};

}  // namespace

std::shared_ptr<media::VideoRendererSinkFactory>
CreateSecondarySinkFactory(std::vector<std::shared_ptr<SceneSource>> sources) {
  return std::make_shared<SceneSecondarySinkFactory>(std::move(sources));
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
