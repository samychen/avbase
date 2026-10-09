// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The two counts docs/07 section 5 names first, at the pipeline level, over
// the SYNTHETIC demuxer, paced to the wall clock at 1x. The path to a passing
// count had three stages, and only the third moved the number:
//
//  1. The graph was stood up on the suite's OWN S3/S4 while inheriting a
//     fixture whose pump drove a DIFFERENT pair. That hung every time; the fix
//     was to go through the base fixture's StartPipeline() and PumpRound()
//     like every other pipeline suite.
//  2. Unpaced, the synthetic demuxer handed out packets as fast as the decoder
//     asked, so the media clock ran AHEAD of real time (measured 3.56 s of
//     media in ~2.5 s of wall) and the compositor correctly presented only the
//     frames the master clock said were due -- 30 frames where 300 were
//     expected. SyntheticSpec::paced + set_tick_clock() fixed the SOURCE.
//  3. With the source paced, the pump had to follow: PumpRound drains three
//     audio periods per 4 ms (~4x realtime), which empties the queues faster
//     than a paced source refills them, so PlayFor() pumps realtime
//     (PumpRoundRealtime) for the clip's full duration rather than a fixed
//     2.5 s.
//
// STILL DISABLED -- the paced source and the blocking pull DEADLOCK, and the
// stack shows exactly where. A paced read parks at the edge (synthetic_demuxer
// AudioStream::Read -> parked_.TimedWait, holding no lock), but it is reached
// only when the decoder stream asks for the next buffer -- and the decoder
// stream asks from S4, which is entered from FakeAudioSink::PullPeriod's
// done.Wait() on the test thread. PullPeriod blocks on that Wait until S4
// finishes one Render, S4 blocks in Read waiting for the wall clock to advance
// past the edge, and the wall clock only advances while the test thread keeps
// pumping -- but the test thread is inside the Wait. Three waiters, no
// progress. The old note's "this suite needs a pump thread (or a tick clock
// the test can advance without draining S1)" is the same answer: PullPeriod
// must not synchronously block the thread that advances time, or the paced
// source must advance against a clock the test drives rather than the wall.
// Until that seam is reworked, enabling these three would ship a hang, which
// is worse than an honest DISABLED.
//
// The supporting change this needed is landed and used elsewhere:
// FakeAudioSink::frames_rendered(), without which the audio count docs/07
// section 5 asks for is not measurable at all -- the pipeline's own statistics
// count what it WROTE, not what left the device.

// The two counts docs/07 section 5 names first, at the pipeline level, over
// the SYNTHETIC demuxer: "play 10 s to a null sink, frames_presented == 300
// +/- 2" and "play 10 s, audio samples out == 480000 +/- one buffer period".
//
// They are the two assertions in that list that can catch a pipeline losing or
// duplicating media somewhere between the demuxer and the sink -- a dropped
// frame, a decoder reset that re-reads, a ring that under-drains -- and they
// are here rather than in a golden comparison because a COUNT needs no
// reference: 10 s at 30 fps is 300 frames, and 10 s at 48 kHz is 480000
// samples. Nothing about the expected value is a matter of opinion.
//
// The tolerance is the interesting part. It is not slack for sloppiness: the
// pump is driven by a real clock, so the LAST frame of a 10 s clip lands
// wherever the clock happened to be when the pump stopped, and the audio total
// is always short by up to one device period because the ring drains into a
// device that has to be pulled to be emptied. A tolerance of zero here would
// be a test that fails for reasons that are not defects.

#include "media/filters/pipeline_impl.h"

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/threading/thread.h"
#include "base/time/default_tick_clock.h"
#include "gtest/gtest.h"
#include "media/base/pipeline_status.h"
#include "tests/support/fake_pipeline_client.h"
#include "tests/support/fake_renderer_sinks.h"
#include "tests/support/fake_sink_factories.h"
#include "tests/support/pipeline_fixture.h"

namespace avbase::media {
namespace {

// 10 s of media at 30 fps and 48 kHz -- the numbers docs/07 section 5 uses.
constexpr int kTargetSeconds = 10;
constexpr int kFps = 30;
constexpr int kExpectedFrames = kTargetSeconds * kFps;    // 300
constexpr int kExpectedSamples = kTargetSeconds * 48000;  // 480000

class PipelineCountsTest : public PipelineTestFixture {
 protected:
  // Real threads and a real clock: the compositor decides what is "due" against
  // the master clock, and a mock clock would make the count an assertion about
  // the test rather than about the pipeline.
  PipelineCountsTest() : PipelineTestFixture(/*ffmpeg_mode=*/false) {
    spec_.width = 320;
    spec_.height = 240;
    spec_.fps_num = kFps;
    spec_.fps_den = 1;
    spec_.duration = base::Seconds(kTargetSeconds);
    // 1x realtime, so "10 s of media" and "10 s of wall" are the same thing
    // and the counts below measure the pipeline rather than the source.
    spec_.paced = true;
  }

  void SetUp() override { PipelineTestFixture::SetUp(); }

  // The BASE fixture's synthetic path, not a hand-built graph. The first
  // version declared its own S3/S4, built its own DefaultRendererFactory and
  // drove its own pump, while inheriting a fixture that pumps a DIFFERENT pair
  // -- and it hung every time. Every other pipeline suite here goes through
  // StartPipeline(); so does this now, which is the whole fix.
  void BuildPipeline() { StartPipeline(); }

  base::TimeDelta PlayFor(base::TimeDelta wall) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(wall.InMilliseconds());
    while (std::chrono::steady_clock::now() < deadline) {
      // Realtime-paced: the source feeds at 1x and parks at the edge, so the
      // pump must let WALL time track MEDIA time. The 4 ms PumpRound drains
      // three audio periods per 4 ms (~4x realtime), which empties the queues
      // faster than a paced source refills them and makes the count an
      // assertion about the pump's overspeed, not about the pipeline.
      PumpRoundRealtime();
      if (client_.ended()) {
        break;
      }
    }
    env_.RunUntilIdle();
    return pipeline_->GetMediaTime();
  }

  std::unique_ptr<AudioBus> bus_;

  int PresentedFrames() const {
    test::FakeVideoSink* sink = video_sinks_->last_sink();
    return sink ? static_cast<int>(sink->frames().size()) : 0;
  }

  // Total audio frames the device actually received, accumulated by the fake
  // sink across every period pulled.
  int64_t RenderedSamples() const {
    test::FakeAudioSink* sink = audio_sinks_->last_sink();
    return sink ? sink->frames_rendered() : 0;
  }
};

// The headline count. A pipeline that drops frames anywhere between demux and
// compositor lands under 300; one that duplicates or re-reads after a reset
// lands over.
TEST_F(PipelineCountsTest,
       DISABLED_TenSecondsOfVideoPresentsThreeHundredFrames) {
  BuildPipeline();
  Play();
  ASSERT_TRUE(client_.HaveMetadata());
  const base::TimeDelta played = PlayFor(spec_.duration);
  const int frames = PresentedFrames();

  EXPECT_NEAR(frames, kExpectedFrames, 2)
      << "presented " << frames << " frames after " << played.ToString()
      << " of a " << spec_.duration.ToString() << " clip (expected "
      << kExpectedFrames << " +/- 2)";
}

// The audio counterpart, with the tolerance docs/07 states: the device holds
// up to one buffer period that has been decoded but not yet pulled.
TEST_F(PipelineCountsTest,
       DISABLED_TenSecondsOfAudioEmitsFourHundredAndEightyThousandSamples) {
  BuildPipeline();
  Play();
  ASSERT_TRUE(client_.HaveMetadata());
  PlayFor(spec_.duration);

  const int64_t samples = RenderedSamples();
  const int64_t one_period = static_cast<int64_t>(kFramesPerBuffer);
  EXPECT_GE(samples, kExpectedSamples - one_period)
      << "emitted only " << samples << " samples of " << kExpectedSamples;
  EXPECT_LE(samples, kExpectedSamples + one_period)
      << "emitted " << samples << " samples, more than the clip contains";
}

// Playback rate must scale the TIMELINE, not the amount of media: 2x covers
// the same 10 s of content in about half the wall time, and therefore presents
// the same number of frames. A rate that silently changed how much was decoded
// would show up here as a count mismatch, which is the failure mode that
// matters (speed changes must not drop or invent media).
TEST_F(PipelineCountsTest,
       DISABLED_DoubleSpeedPresentsTheSameFramesInHalfTheTime) {
  BuildPipeline();
  Play();
  ASSERT_TRUE(client_.HaveMetadata());
  pipeline_->SetPlaybackRate(2.0);
  env_.RunUntilIdle();

  const base::TimeDelta played = PlayFor(base::Milliseconds(2500));
  const int frames = PresentedFrames();

  // Half the clip should have been covered, so half the frames.
  EXPECT_NEAR(played.InMilliseconds(), 5000, 1200)
      << "at 2x, 2.5 s of wall time should cover about 5 s of media, got "
      << played.ToString();
  EXPECT_NEAR(frames, kExpectedFrames / 2, 6)
      << "at 2x the decoder must still see every frame, not half of them";
}

}  // namespace
}  // namespace avbase::media
