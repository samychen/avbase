// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.
//
// A sink that renders nowhere and publishes everything it is handed.
//
// This is the piece the demo could not build while its second picture was a
// delayed copy of the first: a second Player is a whole pipeline of its own --
// its own demuxer, its own decoder, its own clock -- and it still has to
// present SOMEWHERE, because that is the only way its frames get pulled. A sink
// that accepts the pull and drops the frames into a mailbox is what turns one
// player's output into another player's input, which is what "N independent
// decoders" has to mean in a library whose renderer is single-source.
//
// THE MAILBOX IS THE POINT, not the poll loop. Two pipelines running at their
// own rates would otherwise have to be made to agree on one; here they never
// meet, and the display's clock is the only one that decides what is shown.

#ifndef AVBASE_EXAMPLES_PUBLISHER_SINK_H_
#define AVBASE_EXAMPLES_PUBLISHER_SINK_H_

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/render_cadence.h"
#include "media/base/render_perf_stats.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/gl/gl_layer_source.h"

namespace avbase::examples {

// The second stream's endpoint: it draws nothing, and every frame the
// compositor hands it is published into |source| -- which the FIRST player's
// sink drains once per displayed frame.
//
// Owned by the player that created it, like any other sink: Initialize() brings
// the upstream RenderCallback, Start()/Stop() own the poll thread. It is NOT a
// GlVideoSink and never touches a context, which is what makes it usable for a
// source that has no window of its own.
class PublisherSink final : public media::VideoRendererSink {
 public:
  explicit PublisherSink(std::shared_ptr<media::GlLayerSource> source)
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
    // Whatever this source was holding is not on screen any more -- the
    // scene's textures went away with the first player's context -- so the
    // reference goes too. Keeping it would pin the decoder's surface for
    // nothing.
    if (source_) {
      source_->Detach(nullptr);
    }
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

  std::shared_ptr<media::GlLayerSource> source_;
  RenderCallback* upstream_{nullptr};
  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> playing_{false};
};

// Hands every player that asks for one a PublisherSink bound to the same
// source. No window is involved, so there is nothing host-specific left to
// parameterise -- which is the whole reason the second source does not need a
// backend of its own.
class PublisherSinkFactory final : public media::VideoRendererSinkFactory {
 public:
  explicit PublisherSinkFactory(std::shared_ptr<media::GlLayerSource> source)
      : source_(std::move(source)) {}

  std::unique_ptr<media::VideoRendererSink>
  Create(base::scoped_refptr<media::NativeDisplay> /*display*/) override {
    return std::make_unique<PublisherSink>(source_);
  }
  const char* name() const override { return "publisher-factory"; }

 private:
  std::shared_ptr<media::GlLayerSource> source_;
};

}  // namespace avbase::examples

#endif  // AVBASE_EXAMPLES_PUBLISHER_SINK_H_
