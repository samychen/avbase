// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The two counts docs/07 section 5 names first, at the pipeline level, over the
// synthetic demuxer: "play 10 s to a null sink, frames_presented == 300 +/- 2"
// and "play 10 s, audio samples out == 480000 +/- one buffer period". They are
// the two assertions in that list that can catch a pipeline losing or
// duplicating media somewhere between the demuxer and the sink -- a dropped
// frame, a decoder reset that re-reads, a ring that under-drains -- and a COUNT
// needs no reference: 10 s at 30 fps is 300 frames, 10 s at 48 kHz is 480000
// samples. Nothing about the expected value is a matter of opinion.
//
// THE PATH TO A PASSING COUNT had four obstacles, and the first was a real
// defect in the test source:
//
//  1. The graph was stood up on the suite's OWN S3/S4 while inheriting a
//     fixture whose pump drove a DIFFERENT pair. That hung every time; the fix
//     was to go through the base fixture's StartPipeline()/PumpRound() like
//     every other pipeline suite.
//  2. SyntheticDemuxer's VIDEO read unlocked a mutex it had never locked
//     (AudioStream::Read had taken it; VideoStream::Read assumed a
//     single-sequence caller). On Darwin's normal mutexes that does not fail
//     cleanly -- it leaves the mutex claiming a lock nobody holds, and the
//     audio leg, which parks on and relocks the SAME mutex, then blocks forever
//     in lock(). Invisible until pacing routed the video read into that branch.
//     Fixed by holding the lock for the whole read, exactly as the audio leg
//     does.
//  3. The pull was marshalled onto S4, the decode-pump sequence. A paced read
//     parks there until the clock reaches the edge, so every render queued
//     behind that park and the suite ran at a quarter speed. Fixed by giving
//     the fixture a device sequence of its own (S7) -- which is what a real
//     sink has, and what audio_renderer_ring.cc already calls it.
//  4. Wall-clock pacing is not accurate enough here. sleep_until overshoots by
//     up to ~3.8 ms on this platform, and even a busy-wait cannot make a 10 s
//     wall budget land the exact number of 16 ms rounds a count needs. Fixed by
//     driving to the MEDIA target instead: a round that comes back silent
//     advances no media and the loop runs another round, so "10 s of media" is
//     exact and the pump's own speed drops out of the measurement. The pump
//     still paces itself to the wall clock (PumpRoundRealtime) so the source
//     and the display stay in step; only the STOP condition moved.
//
// The 2x case was a fifth obstacle, and clearing it turned up a defect in the
// PIPELINE rather than in the source: a source with the whole file available
// hands the decoder eight packets per read, and the decoder stream was throwing
// seven of them away. It is enabled now; see its own note below.
//
// The tolerance is the interesting part. It is not slack for sloppiness: the
// audio total is short by up to one device period because the ring drains into
// a device that has to be pulled to be emptied, and the video count is a
// DISPLAY count, so a frame the compositor judged not-yet-due is not counted.
// A tolerance of zero would be a test that fails for reasons that are not
// defects.
//
// One supporting change is landed and used elsewhere: FakeAudioSink::
// frames_rendered(), without which the audio count is not measurable at all --
// the pipeline's own statistics count what it WROTE, not what left the device.

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

  // Runs the pump until the pipeline has PLAYED |media| of content.
  //
  // Deliberately a MEDIA target, not a wall-clock budget. The two are the same
  // thing only while the pump keeps up with the 1x source, and "keeps up" is
  // exactly what a count assertion is measuring -- driving to a wall budget
  // folds the pump's own speed into the expected value, so a pump running two
  // per cent slow covers two per cent less media and the count comes up short
  // for a reason that has nothing to do with the pipeline. Driving to the media
  // target removes that term: a round that comes back silent advances no media
  // and the loop simply runs another round.
  base::TimeDelta PlayFor(base::TimeDelta media) {
    // A wall-clock guard so a pipeline that stops advancing media cannot spin
    // here forever; generous, because the paced source sets the floor.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int rounds = 0;
    while (!client_.ended() && pipeline_->GetMediaTime() < media &&
           std::chrono::steady_clock::now() < deadline) {
      PumpRoundRealtime();
      ++rounds;
    }
    env_.RunUntilIdle();
    // Kept so a failing count can say how much pumping it took to get there. An
    // assertion that comes up short for a pipeline reason and one that comes up
    // short because the pump hit its wall-clock guard look identical in the
    // numbers alone.
    play_rounds_ = rounds;
    return pipeline_->GetMediaTime();
  }

  std::unique_ptr<AudioBus> bus_;
  // How many pump rounds the last PlayFor() took, reported in failure messages.
  int play_rounds_ = 0;

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
TEST_F(PipelineCountsTest, TenSecondsOfVideoPresentsThreeHundredFrames) {
  BuildPipeline();
  Play();
  ASSERT_TRUE(client_.HaveMetadata());
  const base::TimeDelta played = PlayFor(spec_.duration);
  const int frames = PresentedFrames();

  EXPECT_NEAR(frames, kExpectedFrames, 2)
      << "presented " << frames << " frames after " << played.ToString()
      << " of a " << spec_.duration.ToString() << " clip (expected "
      << kExpectedFrames << " +/- 2, pump rounds " << play_rounds_ << ")";
}

// The audio counterpart, with the tolerance docs/07 states: the device holds
// up to one buffer period that has been decoded but not yet pulled.
TEST_F(PipelineCountsTest,
       TenSecondsOfAudioEmitsFourHundredAndEightyThousandSamples) {
  BuildPipeline();
  Play();
  ASSERT_TRUE(client_.HaveMetadata());
  PlayFor(spec_.duration);

  const int64_t samples = RenderedSamples();
  const int64_t one_period = static_cast<int64_t>(kFramesPerBuffer);
  EXPECT_GE(samples, kExpectedSamples - one_period)
      << "emitted only " << samples << " samples of " << kExpectedSamples
      << " (pump rounds " << play_rounds_ << ")";
  EXPECT_LE(samples, kExpectedSamples + one_period)
      << "emitted " << samples << " samples, more than the clip contains"
      << " (pump rounds " << play_rounds_ << ")";
}

// Playback rate must scale the TIMELINE, not the amount of media: 2x covers
// the same content in about half the WALL time, and presents every frame of it
// rather than every other one. A rate that silently changed how much was
// decoded would show up here as a count mismatch, which is the failure mode
// that matters (speed changes must not drop or invent media).
//
// This case runs on a VOD-SHAPED source, and that is the honest model rather
// than a workaround for the 1x one the other two use. A source paced to the
// wall clock cannot feed a 2x consumer -- "I will not hand over time I have not
// reached" IS the live edge -- while a file on disk has every packet available
// from the start. In this double that shape is |paced = false|: availability is
// the whole file, and the CONSUMER's clock is what decides when a frame is due.
//
// It was expected to need a new rate model in SyntheticDemuxer. It did not. It
// exposed a defect in the pipeline instead, which is why this case is worth
// having: with the whole file available the demuxer hands over kBuffersPerRead
// (8) packets per read rather than the one-at-a-time the paced leg offers, and
// DecoderStream discarded the un-decoded TAIL of every batch that the decode
// watermark cut short -- seven frames in eight, thrown away between demux and
// compositor. The run then presented 35 frames of the 150 it was asked for and
// looked exactly like "2x drops frames". Fixed in DecodeNextBuffer(); the bug
// was unreachable for as long as every suite fed the decoder one buffer at a
// time, which is what a paced source does by construction.
//
// The wall figure is measured but deliberately NOT asserted, and the reason is
// worth stating: this harness pumps a synthetic display from the test thread,
// so the wall time of a round is set by the PUMP's own cost, not by the
// pipeline's rate. Under ASan the instrumentation stretched the same run to
// 4.6 s of wall for the same 2.5 s of media -- the counts below were unchanged,
// only the machine moved. Asserting a wall figure here would make this a test
// of the build type.
//
// The rate is asserted where it can be measured exactly: the DEVICE COUNT. At
// 2x one device period covers twice the source frames, so five seconds of media
// leaves the device as 2.5 s of audio. At 1x the same run emits 240000 frames;
// 120000 is the 2x answer. That is the same kind of reference-free count the
// other two cases use.
TEST_F(PipelineCountsTest, DoubleSpeedPresentsTheSameFramesInHalfTheTime) {
  // The VOD shape. Set before BuildPipeline(), which builds the demuxer.
  spec_.paced = false;
  BuildPipeline();
  Play();
  ASSERT_TRUE(client_.HaveMetadata());
  pipeline_->SetPlaybackRate(2.0);
  env_.RunUntilIdle();

  const auto wall_start = std::chrono::steady_clock::now();
  const base::TimeDelta played = PlayFor(base::Seconds(5));
  const auto wall = std::chrono::steady_clock::now() - wall_start;
  const auto wall_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(wall).count();
  const int frames = PresentedFrames();
  const int64_t samples = RenderedSamples();

  EXPECT_NEAR(played.InMilliseconds(), 5000, 200)
      << "the run should have covered the 5 s it was asked for, got "
      << played.ToString();
  // kExpectedSamples is the 1x figure (10 s of audio); a quarter of it is the
  // 2.5 s the device is handed while covering 5 s of media at 2x. The tolerance
  // is not slack for sloppiness and it is not zero either, and the reason is
  // measurable: the loop stops when the audio CLOCK reports 5 s, and that clock
  // is EXTRAPOLATED between device reports, so it can be tens of milliseconds
  // ahead of what the device has actually been handed. Under ASan with the rest
  // of the suite running in parallel the shortfall was 1184 frames (1%). What
  // this distinguishes from is 240000 -- a factor of two away -- so 5% decides
  // nothing that matters while it absorbs the clock.
  EXPECT_NEAR(samples, kExpectedSamples / 4, kExpectedSamples / 20)
      << "at 2x the device should be handed half as many output frames per"
      << " second of media; got " << samples << " for " << played.ToString()
      << " of media (wall " << wall_ms << " ms, pump rounds " << play_rounds_
      << ")";
  EXPECT_NEAR(frames, kExpectedFrames / 2, 6)
      << "at 2x the decoder must still see every frame, not half of them"
      << " (wall " << wall_ms << " ms, pump rounds " << play_rounds_ << ")";
}

}  // namespace
}  // namespace avbase::media
