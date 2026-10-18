// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The null audio sink's re-arm interval, measured against wall time. The sink
// has no device to block on, so how long it waits between periods is a choice
// this file exists to pin down: waiting too little turns every underrun into a
// spin, and a headless sink that burns a core on silence is the kind of number
// that gets read as "the codec is slow" weeks later.

#include "media/filters/null_audio_sink.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"

namespace avbase::media {
namespace {

constexpr int kSampleRate = 48000;
// 512 frames at 48 kHz is a 10.67 ms period: ~94 periods per wall second.
constexpr int kFramesPerPeriod = 512;

// Plays silence forever: the "no data available" answer the sink's own
// contract documents, and what a starved or paused renderer returns.
class StarvedCallback final : public AudioRendererSink::RenderCallback {
 public:
  int Render(base::TimeDelta /*delay*/, base::TimeTicks /*delay_timestamp*/,
             const AudioGlitchInfo& /*glitch_info*/,
             AudioBus* /*dest*/) override {
    const base::TimeTicks now = base::TimeTicks::Now();
    std::lock_guard<std::mutex> lock(mu_);
    // The smallest gap between two consecutive Render() calls is what betrays
    // the re-arm interval: a real device period is >= 10 ms, the spin floor is
    // 21 us. Measuring the interval (not a wall-clock count) keeps the
    // assertion load-independent -- a busy CI box lengthens a 10 ms period a
    // little, it never shrinks it toward microseconds.
    if (prev_.is_null()) {
      prev_ = now;
    } else {
      const base::TimeDelta gap = now - prev_;
      prev_ = now;
      if (min_gap_.is_zero() || gap < min_gap_) {
        min_gap_ = gap;
      }
    }
    ++calls_;
    return 0;
  }
  void OnRenderError() override {}

  int calls() const {
    std::lock_guard<std::mutex> lock(mu_);
    return calls_;
  }
  base::TimeDelta min_gap() const {
    std::lock_guard<std::mutex> lock(mu_);
    return min_gap_;
  }

 private:
  mutable std::mutex mu_;
  base::TimeTicks prev_;
  base::TimeDelta min_gap_;
  int calls_ = 0;
};

AudioParameters StarvedParams() {
  return AudioParameters(ChannelLayout::kStereo, SampleFormat::kF32P,
                         kSampleRate, kFramesPerPeriod);
}

}  // namespace

// A period that produced nothing still costs a period: a real device takes
// 10.67 ms whether or not Render() filled the buffer, so this sink must too.
// Sleeping for what was produced instead would re-arm on max(0, 1)/sample_rate
// -- 21 us at 48 kHz -- which is one Render() per sample: 48 kHz of callbacks,
// a core burned for as long as the source stays starved. That is what a
// headless run does at the end of a 2x stream whose tail never drains, and it
// is what made one 3 s file cost 4.5 s of CPU instead of 0.5 s.
TEST(NullAudioSinkTest, AStarvedPeriodStillCostsAFullPeriod) {
  StarvedCallback callback;
  NullAudioSink sink;
  sink.Initialize(StarvedParams(), &callback);
  sink.Start();
  sink.Play();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  sink.Stop();

  // The sink must have actually run, or the interval check below is vacuous.
  ASSERT_GE(callback.calls(), 5)
      << "the sink did not render at all while playing";
  // The 10.67 ms device period, rounded down generously to 1 ms, separates the
  // correct behaviour from the 21 us spin floor by ~500x -- so this holds on a
  // loaded machine yet still fails loudly if Render() re-arms per sample.
  std::printf("starved sink: %d calls, min gap %lld us\n", callback.calls(),
              callback.min_gap().InMicroseconds());
  EXPECT_GE(callback.min_gap(), base::Milliseconds(1))
      << "the sink re-arms faster than the device period when Render() "
         "produces nothing";
}

// Not playing is the idle poll, not the device period: 20 ms, so Play() feels
// immediate and a stopped sink costs nothing. Asserted as "never at device
// rate, and never spinning" rather than as an exact count, because the whole
// point of the idle path is that its period is not load-bearing.
TEST(NullAudioSinkTest, NotPlayingPollsInsteadOfRendering) {
  StarvedCallback callback;
  NullAudioSink sink;
  sink.Initialize(StarvedParams(), &callback);
  sink.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  sink.Stop();

  std::printf("idle sink: %d Render() calls in 300 ms\n", callback.calls());
  EXPECT_EQ(callback.calls(), 0) << "the sink rendered while not playing";
}

}  // namespace avbase::media
