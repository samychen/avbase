// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The M9 stall-recovery DoD at the pipeline level, over the REAL FFmpegDemuxer:
// the DataSource bridge feeds it from a ThrottledDataSource, and the renderer's
// starvation signal must report the dry edge and the recovery to the client, in
// that order. No component-level suite can see this chain.
//
// RE-ENABLED, and the two things that made it possible were both arrived at
// backwards, so they are worth naming.
//
// 1. A 40 s, LOW-BITRATE asset (tests/testdata/long_lowbr_40s.mp4, 514 KB,
//    ~12.9 KB/s). The previous asset was 3 s of ~109 KB/s, and NO throttle
//    setting could express the cycle on it: above consumption the queues
//    refill (no dry edge), below it they drain and never refill (no recovery).
//    A 3 s clip cannot hold a full queue long enough to observe the transition
//    at all -- the earlier runs proved that, measuring a clean "ended" with no
//    starvation whatsoever. 40 s of 12.9 KB/s gives the two regimes room to be
//    told apart instead of racing at the boundary.
//
// 2. A RATE THAT CHANGES MID-STREAM, which is the deeper of the two. Even with
//    the right asset, a CONSTANT rate cannot produce dry-then-recover: it
//    either starves forever or never starves. What produces the cycle is what
//    happens on a real network -- the link degrades, then it comes back. So the
//    test starves the pipeline, waits for the dry edge, restores the rate, and
//    waits for recovery. The budget is deliberately NOT refilled on the change,
//    so recovery has to be earned by the new rate instead of bought with a
//    burst.
//
// What this asserts is the CYCLE and its ORDER. The inverse matters too: a
// suite that only proved "it starves" would also pass on a pipeline that simply
// wedged.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/threading/thread.h"
#include "base/time/default_tick_clock.h"
#include "gtest/gtest.h"
#include "media/base/data_source.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/pipeline_status.h"
#include "media/filters/pipeline_impl.h"
#include "tests/support/fake_pipeline_client.h"
#include "tests/support/fake_renderer_sinks.h"
#include "tests/support/fake_sink_factories.h"
#include "tests/support/pipeline_fixture.h"
#include "tests/support/throttled_data_source.h"

namespace avbase::media {
namespace {

[[maybe_unused]] constexpr auto kWaitTimeout = std::chrono::seconds(45);
[[maybe_unused]] constexpr int kFramesPerBuffer = 256;
[[maybe_unused]] constexpr int kAudioChannels = 2;

// Below the ~12.9 KB/s the clip needs, so the queues genuinely drain.
constexpr int kStalledBytesPerSecond = 8 * 1024;
// Comfortably above it, so the link coming back is unambiguous.
constexpr int kRecoveredBytesPerSecond = 256 * 1024;

}  // namespace

class PipelineThrottleTest : public PipelineTestFixture {
 protected:
  PipelineThrottleTest() : PipelineTestFixture(/*ffmpeg_mode=*/true) {
    media_file_ = "long_lowbr_40s.mp4";
  }
};

TEST_F(PipelineThrottleTest, ThrottledSourceProducesAStallRecoverCycle) {
  LoadMediaBytes();
  auto memory = base::MakeRefCounted<MemoryDataSource>(media_bytes_.data(),
                                                       media_bytes_.size());
  auto throttled = base::MakeRefCounted<test::ThrottledDataSource>(
      std::move(memory), kStalledBytesPerSecond);
  throttled->set_max_burst_bytes(kStalledBytesPerSecond);
  // Kept as a raw pointer BEFORE the move: StartPipeline takes ownership, and
  // the test needs to change the rate afterwards.
  test::ThrottledDataSource* const source = throttled.get();
  StartPipeline(std::move(throttled));

  // Every failure message carries the source's own counters. "no kHaveNothing"
  // cannot be told apart from "the throttle never engaged", and that ambiguity
  // is what parked this test for three rounds.
  const auto describe = [this, source] {
    return "bytes_served=" + std::to_string(source->bytes_served()) +
           " stalls=" + std::to_string(source->stalls()) +
           " media_time=" + pipeline_->GetMediaTime().ToString() +
           "; events:\n" + client_.EventLog();
  };

  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }))
      << "never probed the throttled source; " << describe();
  pipeline_->Play();
  ASSERT_TRUE(PumpUntil([this] {
    PumpRoundRealtime();
    return video_sinks_->last_sink() && audio_sinks_->last_sink() &&
           video_sinks_->last_sink()->start_count() > 0 &&
           audio_sinks_->last_sink()->start_count() > 0;
  })) << "sinks never started; "
      << describe();
  audio_sinks_->last_sink()->set_render_runner(audio_thread_.task_runner());

  // The link is slow: the queues must run dry.
  ASSERT_TRUE(PumpUntil([this] {
    PumpRoundRealtime();
    return client_.have_nothing();
  })) << "no kHaveNothing while the link was slow; "
      << describe();

  // The link comes back. The budget is not refilled, so recovery has to be
  // earned by the new rate rather than handed over.
  source->set_bytes_per_second(kRecoveredBytesPerSecond);
  ASSERT_TRUE(PumpUntil([this] {
    PumpRoundRealtime();
    return client_.have_enough();
  })) << "no kHaveEnough after the link recovered; "
      << describe();

  EXPECT_FALSE(client_.HasError()) << client_.error().ToString();
}

}  // namespace avbase::media
