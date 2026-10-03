// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The M9 stall-recovery DoD ("throttled 50KB/s: the stall-recover cycle
// works") at the pipeline level, over the REAL FFmpegDemuxer: the
// DataSource bridge feeds it from a ThrottledDataSource, the renderer's
// starvation signal must report the dry edge and the recovery to the
// client, in that order. No component-level suite can see this chain.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
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

constexpr auto kWaitTimeout = std::chrono::seconds(45);
constexpr int kFramesPerBuffer = 256;
constexpr int kAudioChannels = 2;

}  // namespace

class PipelineThrottleTest : public PipelineTestFixture {
 protected:
  PipelineThrottleTest() : PipelineTestFixture(/*ffmpeg_mode=*/true) {
    media_file_ = "small_h264_aac_3s.mp4";
  }
};

// A real MP4 served through a throttled DataSource, asserting the M9
// stall-recovery DoD: the queues run dry mid-stream, and the renderer reports
// starvation and then recovery.
//
// STILL DISABLED, but no longer for the reasons recorded before. This round
// fixed two real defects on the way -- both in the shared fixture, both found
// only because this test was re-enabled and actually run -- and then
// established, with numbers rather than a shrug, that the remaining blocker
// is the TEST MEDIA and not the fixture, the pump, or the buffering logic.
//
//   FIXED 1 (use-after-free, shared fixture). StartPipeline() reloaded
//   media_bytes_ unconditionally, reallocating the vector AFTER a caller had
//   built a MemoryDataSource over media_bytes_.data(). This suite is the only
//   one that wraps the source, which is why only it saw the fallout: FFmpeg
//   reported 46 consecutive "Invalid data found when processing input" and the
//   suite misread that as a pacing problem, when in fact the decoder was
//   being handed freed memory. Both LoadMediaBytes() and StartPipeline() now
//   load only while the vector is still empty.
//
//   FIXED 2 (data race, shared fixture). FakePipelineClient::have_nothing()
//   and have_enough() read their flags without the mutex that every other
//   accessor takes, while the media sequence writes them.
//
//   BLOCKER (test media). The cycle needs a source slow enough to starve and
//   fast enough to recover, and no asset currently in reach can express that.
//   small_h264_aac_3s.mp4 is 327 KB of 3 s media, i.e. ~109 KB/s to play in
//   real time:
//     * above ~110 KB/s the whole container arrives inside those 3 s, the
//       queues never run dry, and the run ends cleanly with no starvation at
//       all (measured: bytes_served=371692, media_time=60s on a 3 s file --
//       the demuxer read far ahead of the renderers, which is the decoder
//       queue's back-pressure working as designed);
//     * at the 60 KB/s this test originally used, starvation is PERMANENT, so
//       kHaveEnough is arithmetically unreachable -- that was the previously
//       recorded reason for parking it;
//     * in between, whether a DRY edge appears at all depends on whether the
//       demuxer's read-ahead outruns the renderers, which is a race rather
//       than a specification.
//   A 30 s+ source separates the regimes cleanly: at 60 KB/s a 30 s clip
//   needs ~150 s to deliver, so it starves early and recovers late, with both
//   edges far from either boundary. Until such an asset exists, re-enabling
//   this means picking whatever number passes today and freezing it into the
//   test -- which is the same mistake each of the three earlier parked
//   diagnoses made, in turn.
//
// The fixture work this test motivated IS landed and stays landed: the
// realtime-paced PumpRoundRealtime variant, and the diagnostics helper, which
// reports bytes served and stall count so the next attempt does not have to
// re-derive any of the above.
TEST_F(PipelineThrottleTest,
       DISABLED_ThrottledSourceProducesAStallRecoverCycle) {
  LoadMediaBytes();
  auto memory = base::MakeRefCounted<MemoryDataSource>(media_bytes_.data(),
                                                       media_bytes_.size());
  auto throttled = base::MakeRefCounted<test::ThrottledDataSource>(
      std::move(memory), 256 * 1024);
  throttled->set_max_burst_bytes(16 * 1024);
  // The pipeline takes ownership, and StartPipeline(std::move(...)) leaves
  // |throttled| NULL -- so the diagnostics below must hold a separate
  // non-owning pointer taken BEFORE the move. They first shipped capturing
  // the refptr by reference, which CHECK-failed on that null refptr the
  // moment the first failure message was built.
  test::ThrottledDataSource* const throttled_raw = throttled.get();
  StartPipeline(std::move(throttled));

  // Carried into every failure message. Without the served-byte and stall
  // counts, "no kHaveEnough" cannot be told apart from "the throttle never
  // engaged at all" -- which is exactly the question this test was failing on
  // for three rounds, and the reason the diagnosis took that long.
  const auto describe_source = [this, throttled_raw] {
    return "bytes_served=" + std::to_string(throttled_raw->bytes_served()) +
           " stalls=" + std::to_string(throttled_raw->stalls()) +
           " media_time=" + pipeline_->GetMediaTime().ToString() +
           "; events:\n" + client_.EventLog();
  };

  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }))
      << "never probed the throttled source; " << describe_source();
  pipeline_->Play();
  ASSERT_TRUE(PumpUntil([this] {
    PumpRoundRealtime();
    return video_sinks_->last_sink() && audio_sinks_->last_sink() &&
           video_sinks_->last_sink()->start_count() > 0 &&
           audio_sinks_->last_sink()->start_count() > 0;
  })) << "sinks never started; "
      << describe_source();
  audio_sinks_->last_sink()->set_render_runner(audio_thread_.task_runner());

  // The throttle guarantees the queues run dry: the burst is far too small to
  // hold the whole file.
  ASSERT_TRUE(PumpUntil([this] {
    PumpRoundRealtime();
    return client_.have_nothing();
  })) << "no kHaveNothing under a throttled source; "
      << describe_source();
  // And the recovery edge must follow -- playback continues, it does not
  // end in starvation.
  ASSERT_TRUE(PumpUntil([this] {
    PumpRoundRealtime();
    return client_.have_enough();
  })) << "no kHaveEnough after starvation; events:\n"
      << client_.EventLog();
  EXPECT_FALSE(client_.HasError()) << client_.error().ToString() << "\n"
                                   << client_.EventLog();
}

}  // namespace avbase::media
