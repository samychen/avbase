// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/fake_renderer_sinks.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/synchronization/waitable_event.h"

namespace avbase::media::test {

void FakeVideoSink::Initialize(RenderCallback* callback) {
  callback_.store(callback);
}

void FakeVideoSink::Start() {
  ++start_count_;
  running_.store(true);
}

void FakeVideoSink::Stop() {
  ++stop_count_;
  running_.store(false);
}

void FakeVideoSink::Pause() {
  ++pause_count_;
  running_.store(false);
}

void FakeVideoSink::Play() {
  ++play_count_;
  running_.store(true);
}

void FakeVideoSink::Flush() { ++flush_count_; }

void FakeVideoSink::SetOutputTarget(
    base::scoped_refptr<NativeDisplay> /*display*/) {}

bool FakeVideoSink::GetDisplayInterval(base::TimeDelta* interval) const {
  *interval = display_interval_;
  return true;
}

VideoSinkStats FakeVideoSink::GetStats() const {
  VideoSinkStats stats;
  stats.frames_presented = frames_.size();
  return stats;
}

int FakeVideoSink::PullFrames(int max_frames) {
  RenderCallback* callback = callback_.load();
  if (!callback) {
    return 0;
  }
  int presented = 0;
  for (int i = 0; i < max_frames; ++i) {
    // A synthetic but monotonically advancing display: the compositor decides
    // what to present from the deadline pair, so a frozen one would keep
    // answering "repeat the previous frame".
    const base::TimeTicks min =
        base::TimeTicks() + base::Milliseconds(pull_count_ * 16);
    const base::TimeTicks max = min + display_interval_;
    ++pull_count_;
    base::scoped_refptr<VideoFrame> frame = callback->Render(min, max);
    if (frame) {
      frames_.push_back(std::move(frame));
      ++presented;
    }
  }
  return presented;
}

void FakeAudioSink::Initialize(const AudioParameters& params,
                               RenderCallback* callback) {
  params_ = params;
  callback_.store(callback);
}

void FakeAudioSink::Start() {
  ++start_count_;
  running_.store(true);
}

void FakeAudioSink::Stop() {
  ++stop_count_;
  running_.store(false);
  // Clear the callback, honouring the real sink contract ("Render() is never
  // called after Stop() returns") against this fixture's own PullPeriod(),
  // which pulls from the test thread and can run between a renderer teardown
  // and the replacement's Initialize(). Without this, the callback pointer is
  // a dangling reference into the deleted renderer.
  callback_.store(nullptr);
}

void FakeAudioSink::Pause() {
  ++pause_count_;
  running_.store(false);
}

void FakeAudioSink::Play() {
  ++play_count_;
  running_.store(true);
}

void FakeAudioSink::Flush() { ++flush_count_; }

bool FakeAudioSink::SetVolume(double volume) {
  volume_.store(volume);
  return true;
}

int FakeAudioSink::PullPeriod(AudioBus* dest) {
  RenderCallback* callback = callback_.load();
  if (!callback || !dest) {
    return 0;
  }
  if (!render_runner_) {
    return callback->Render(base::TimeDelta(), base::TimeTicks(),
                            AudioGlitchInfo(), dest);
  }
  // One device period, rendered on the renderer's own sequence. The caller
  // blocks until it is done, which is what makes this equivalent to a device
  // thread pulling at this instant -- and what keeps every renderer state
  // touch on one sequence.
  base::WaitableEvent done;
  int written = 0;
  render_runner_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](RenderCallback* cb, AudioBus* bus, int* out,
                        base::WaitableEvent* ev) {
                       *out = cb->Render(base::TimeDelta(), base::TimeTicks(),
                                         AudioGlitchInfo(), bus);
                       ev->Signal();
                     },
                     callback, dest, &written, &done));
  done.Wait();
  return written;
}

}  // namespace avbase::media::test
