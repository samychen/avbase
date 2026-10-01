// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/null_audio_sink.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/synchronization/lock.h"

namespace avbase::media {
namespace {

// How long the idle loop waits when the sink is not playing. Short enough
// that Play() feels immediate, long enough to be free.
constexpr base::TimeDelta kIdlePoll = base::Milliseconds(20);

}  // namespace

NullAudioSink::NullAudioSink() = default;

NullAudioSink::~NullAudioSink() {
  // Stop() joins the audio thread first: the contract this backend must keep
  // is that Render() is never called after Stop() returns, which is also what
  // makes AudioRendererImpl's teardown order safe.
  Stop();
}

void NullAudioSink::Initialize(const AudioParameters& params,
                               RenderCallback* callback) {
  params_ = params;
  callback_ = callback;
  bus_ = AudioBus::Create(params.channels(), params.frames_per_buffer());
  bus_->Zero();
}

void NullAudioSink::Start() {
  if (started_.exchange(true)) {
    return;
  }
  thread_ = std::make_unique<base::Thread>("avbase-null-audio");
  thread_->Start();
  audio_runner_ = thread_->task_runner();
  audio_runner_->PostTask(FROM_HERE,
                          base::BindOnce(&NullAudioSink::ConsumeOnePeriod,
                                         base::Unretained(this)));
}

void NullAudioSink::Stop() {
  started_.store(false);
  playing_.store(false);
  if (thread_) {
    thread_->Stop();
    thread_.reset();
    audio_runner_ = nullptr;
  }
}

void NullAudioSink::Pause() {
  playing_.store(false);
}

void NullAudioSink::Play() {
  if (started_.load()) {
    playing_.store(true);
  }
}

void NullAudioSink::Flush() {
  // Only valid while not playing, per the AudioRendererSink contract;
  // AudioRendererImpl::Flush() guarantees that ordering.
}

bool NullAudioSink::SetVolume(double volume) {
  (void)volume;   // Nothing is played; there is nothing to scale.
  return true;
}

bool NullAudioSink::IsOptimizedForHardwareParameters() {
  return false;   // Any parameters work; nothing is opened.
}

bool NullAudioSink::CurrentThreadIsRenderingThread() {
  return on_rendering_thread_.load();
}

base::TimeDelta NullAudioSink::GetHardwareLatency() const {
  return base::TimeDelta();
}

AudioGlitchInfo NullAudioSink::GetGlitchInfo() const {
  base::AutoLock scoped(glitch_lock_);
  return glitch_info_;
}

void NullAudioSink::ConsumeOnePeriod() {
  on_rendering_thread_.store(true);
  base::TimeDelta wait = kIdlePoll;
  const int requested = bus_ ? bus_->frames() : 0;
  if (playing_.load() && callback_ && requested > 0) {
    // delay/delay_timestamp describe when the first produced sample would hit
    // the (fictional) speaker: one full period from now, zero hardware latency.
    const AudioGlitchInfo none;
    const base::TimeTicks now = base::TimeTicks::Now();
    const int written = callback_->Render(params_.buffer_duration(), now,
                                          none, bus_.get());
    if (written < requested) {
      base::AutoLock scoped(glitch_lock_);
      glitch_info_.total_glitches += 1;
      glitch_info_.total_glitch_duration += params_.buffer_duration();
    }
    // Sleep for what was actually produced, so a starved source is consumed in
    // real time rather than being overtaken by the null clock.
    wait = base::SecondsD(static_cast<double>(std::max(written, 1)) /
                          static_cast<double>(params_.sample_rate()));
  }
  if (audio_runner_ && started_.load()) {
    audio_runner_->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&NullAudioSink::ConsumeOnePeriod,
                       base::Unretained(this)),
        wait);
  }
}

base::scoped_refptr<AudioRendererSink> NullAudioSinkFactory::Create() {
  return base::MakeRefCounted<NullAudioSink>();
}

}  // namespace avbase::media
