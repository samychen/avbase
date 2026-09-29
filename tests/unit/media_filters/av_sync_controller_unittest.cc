// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Tests for the ported ffplay synchronisation algorithms. Every threshold here
// matches AV_DIFF_AVG_COEF / AV_DIFF_AVG_NB / AV_DIFF_THRESHOLD /
// SAMPLE_CORRECTION_PERCENT_MAX / AV_NOSYNC_THRESHOLD from ff_ffplay_def.h.
// If those constants change upstream, tools/extract_constants.py must be re-run
// and these expectations revisited — do not "fix" the test to match a guess.

#include "media/filters/legacy/av_sync_controller.h"

#include <atomic>
#include <thread>
#include <vector>

#include "base/time/simple_test_tick_clock.h"
#include "gtest/gtest.h"

namespace ijkpp::media {
namespace {

using M = AvSyncController::MasterType;

class AvSyncControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    controller_ = std::make_unique<AvSyncController>(
        M::kAudio, &clock_, AvSyncController::Thresholds{});
  }
  base::SimpleTestTickClock clock_;
  std::unique_ptr<AvSyncController> controller_;
};

// ---- master clock selection (get_master_sync_type) -------------------------

TEST_F(AvSyncControllerTest, AudioMasterIsUsedWhenAudioExists) {
  controller_->SetStreamAvailability(/*has_audio=*/true, /*has_video=*/true);
  controller_->OnAudioFramesConsumed(1024, base::Seconds(5), 1);
  EXPECT_EQ(controller_->ResolveMasterType(), M::kAudio);
  EXPECT_EQ(controller_->GetMasterClock(), base::Seconds(5));
  EXPECT_TRUE(controller_->master_clock_valid());
}

TEST_F(AvSyncControllerTest, AudioMasterFallsBackToVideoWithoutAnAudioStream) {
  controller_->SetStreamAvailability(/*has_audio=*/false, /*has_video=*/true);
  controller_->OnVideoFramePresented(base::Seconds(7), 1);
  // ffplay: audio master requested but no audio stream -> use video.
  EXPECT_EQ(controller_->ResolveMasterType(), M::kVideo);
  EXPECT_EQ(controller_->GetMasterClock(), base::Seconds(7));
}

TEST_F(AvSyncControllerTest, VideoMasterFallsBackToAudioWithoutAVideoStream) {
  controller_->set_requested_master(M::kVideo);
  controller_->SetStreamAvailability(/*has_audio=*/true, /*has_video=*/false);
  controller_->OnAudioFramesConsumed(1024, base::Seconds(3), 1);
  EXPECT_EQ(controller_->ResolveMasterType(), M::kAudio);
  EXPECT_EQ(controller_->GetMasterClock(), base::Seconds(3));
}

TEST_F(AvSyncControllerTest, ExternalMasterIsNeverRedirected) {
  controller_->set_requested_master(M::kExternal);
  controller_->SetStreamAvailability(true, true);
  controller_->SetExternalClock(base::Seconds(9), 4);
  EXPECT_EQ(controller_->ResolveMasterType(), M::kExternal);
  EXPECT_EQ(controller_->GetMasterClock(), base::Seconds(9));
  EXPECT_EQ(controller_->master_serial(), 4);
}

// The fallback that stops a video-only stream from having no clock at all.
TEST_F(AvSyncControllerTest, FallsBackToExternalWhenMasterClockIsInvalid) {
  controller_->SetStreamAvailability(false, false);
  controller_->SetExternalClock(base::Seconds(2), 1);
  EXPECT_FALSE(IsNoTimestamp(controller_->GetMasterClock()));
  EXPECT_EQ(controller_->GetMasterClock(), base::Seconds(2));
}

TEST_F(AvSyncControllerTest, InvalidEverywhereReportsNoClock) {
  controller_->SetStreamAvailability(false, false);
  EXPECT_FALSE(controller_->master_clock_valid());
  EXPECT_TRUE(IsNoTimestamp(controller_->GetMasterClock()));
}

// ---- clock extrapolation ----------------------------------------------------

TEST_F(AvSyncControllerTest, MasterClockAdvancesWithWallTime) {
  controller_->OnAudioFramesConsumed(1024, base::Seconds(10), 1);
  EXPECT_EQ(controller_->GetMasterClock(), base::Seconds(10));
  clock_.Advance(base::Milliseconds(250));
  EXPECT_EQ(controller_->GetMasterClock(), base::Seconds(10) + base::Milliseconds(250));
}

// Regression for bug #32. Clock::Get() reconstructed the master clock from
// `drift` (already pts - capture_instant) plus elapsed wall time, which removes
// the capture instant twice. With a test clock starting at zero the two forms
// agree, so this only shows up once the clock origin is non-zero -- i.e. in a
// real process, where the offset equals negative uptime.
TEST_F(AvSyncControllerTest, MasterClockIsUnaffectedByWallClockOrigin) {
  // A host that has been up for 9.2 hours, matching the `ijkpp-inspect sync`
  // run that exposed this.
  clock_.Advance(base::Seconds(33185));
  controller_->SetStreamAvailability(/*has_audio=*/true, /*has_video=*/true);
  controller_->OnAudioFramesConsumed(1024, base::Seconds(10), 1);
  EXPECT_EQ(controller_->GetMasterClock(), base::Seconds(10))
      << "the master clock must be media time, not offset by wall-clock origin";
  EXPECT_TRUE(controller_->master_clock_valid());

  clock_.Advance(base::Milliseconds(250));
  EXPECT_EQ(controller_->GetMasterClock(),
            base::Seconds(10) + base::Milliseconds(250));
}

TEST_F(AvSyncControllerTest, PlaybackRateExtrapolationSurvivesNonZeroOrigin) {
  clock_.Advance(base::Seconds(33185));
  controller_->OnAudioFramesConsumed(1024, base::Seconds(10), 1);
  controller_->SetPlaybackRate(2.0);
  clock_.Advance(base::Milliseconds(100));
  // 100 ms of wall time at 2x is 200 ms of media time, independent of origin.
  EXPECT_EQ(controller_->GetMasterClock(),
            base::Seconds(10) + base::Milliseconds(200));
}

TEST_F(AvSyncControllerTest, PlaybackRateScalesExtrapolation) {
  controller_->OnAudioFramesConsumed(1024, base::Seconds(10), 1);
  controller_->SetPlaybackRate(2.0);
  clock_.Advance(base::Milliseconds(100));
  // 100 ms of wall time at 2x is 200 ms of media time.
  EXPECT_EQ(controller_->GetMasterClock(), base::Seconds(10) + base::Milliseconds(200));
}

TEST_F(AvSyncControllerTest, AvDiffIsAudioMinusVideo) {
  controller_->OnAudioFramesConsumed(1024, base::Milliseconds(1000), 1);
  controller_->OnVideoFramePresented(base::Milliseconds(980), 1);
  EXPECT_EQ(controller_->av_diff(), base::Milliseconds(20));
}

TEST_F(AvSyncControllerTest, AvDiffIsZeroWhenEitherClockIsInvalid) {
  controller_->OnAudioFramesConsumed(1024, base::Seconds(1), 1);
  EXPECT_EQ(controller_->av_diff(), base::TimeDelta());
}

TEST_F(AvSyncControllerTest, FlushInvalidatesAllClocks) {
  controller_->OnAudioFramesConsumed(1024, base::Seconds(5), 1);
  controller_->OnVideoFramePresented(base::Seconds(5), 1);
  controller_->SetExternalClock(base::Seconds(5), 1);
  ASSERT_TRUE(controller_->master_clock_valid());
  controller_->Flush();
  EXPECT_FALSE(controller_->master_clock_valid());
  EXPECT_FALSE(controller_->audio_clock().valid());
  EXPECT_FALSE(controller_->video_clock().valid());
}

// ---- synchronize_audio port -------------------------------------------------

TEST_F(AvSyncControllerTest, NoCorrectionWhileAudioIsTheMaster) {
  // ffplay only resamples audio when it is the slave. Correcting the master
  // would fight the reference the whole pipeline is following.
  controller_->OnAudioFramesConsumed(1024, base::Seconds(1), 1);
  controller_->OnVideoFramePresented(base::Seconds(10), 1);
  EXPECT_EQ(controller_->ResolveMasterType(), M::kAudio);
  EXPECT_EQ(controller_->ComputeAudioSampleAdjustment(1024, 48000), 0);
}

TEST_F(AvSyncControllerTest, NoCorrectionUntilTheDiffWindowIsFull) {
  controller_->set_requested_master(M::kVideo);
  controller_->OnVideoFramePresented(base::Seconds(10), 1);
  controller_->OnAudioFramesConsumed(1024, base::Seconds(9), 1);
  // AV_DIFF_AVG_NB is 10: the first nine samples only fill the window.
  for (int i = 0; i < 9; ++i) {
    EXPECT_EQ(controller_->ComputeAudioSampleAdjustment(1024, 48000), 0);
    controller_->OnAudioFramesConsumed(1024, base::Seconds(9), 1);
  }
}

TEST_F(AvSyncControllerTest, CorrectsOnceTheWindowIsFullAndDriftExceedsThreshold) {
  controller_->set_requested_master(M::kVideo);
  controller_->OnVideoFramePresented(base::Seconds(10), 1);
  // Audio a full second behind video: far above AV_DIFF_THRESHOLD (0.1 s).
  controller_->OnAudioFramesConsumed(1024, base::Seconds(9), 1);
  int adjustment = 0;
  for (int i = 0; i < 40; ++i) {
    adjustment = controller_->ComputeAudioSampleAdjustment(1024, 48000);
    controller_->OnAudioFramesConsumed(1024, base::Seconds(9), 1);
    if (adjustment != 0) break;
  }
  EXPECT_NE(adjustment, 0);
  // SAMPLE_CORRECTION_PERCENT_MAX is 10. ffplay computes the bound with integer
  // arithmetic — 1024 * 90 / 100 == 921, so the largest shrink is 103 samples,
  // not the 102.4 that floating point would suggest. Asserting -102 here would
  // be asserting a port that differs from the original.
  EXPECT_LT(adjustment, 0);
  EXPECT_GE(adjustment, -103);
}

TEST_F(AvSyncControllerTest, CorrectionIsClampedToTenPercent) {
  controller_->set_requested_master(M::kVideo);
  controller_->OnVideoFramePresented(base::Seconds(100), 1);
  controller_->OnAudioFramesConsumed(1024, base::Seconds(1), 1);   // 99 s behind
  int adjustment = 0;
  for (int i = 0; i < 40; ++i) {
    adjustment = controller_->ComputeAudioSampleAdjustment(1024, 48000);
    controller_->OnAudioFramesConsumed(1024, base::Seconds(1), 1);
    if (adjustment != 0) break;
  }
  EXPECT_GE(adjustment, -103);
  EXPECT_LE(adjustment, 103);
}

TEST_F(AvSyncControllerTest, NoCorrectionBeyondNoSyncThreshold) {
  controller_->set_requested_master(M::kVideo);
  // AV_NOSYNC_THRESHOLD is 10 s: past that, pacing/resampling gives up and the
  // caller snaps the clock instead.
  controller_->OnVideoFramePresented(base::Seconds(100), 1);
  controller_->OnAudioFramesConsumed(1024, base::Seconds(1), 1);
  for (int i = 0; i < 40; ++i) {
    EXPECT_EQ(controller_->ComputeAudioSampleAdjustment(1024, 48000), 0);
    controller_->OnAudioFramesConsumed(1024, base::Seconds(1), 1);
  }
}

TEST_F(AvSyncControllerTest, NoCorrectionForDegenerateInputs) {
  controller_->set_requested_master(M::kVideo);
  controller_->OnVideoFramePresented(base::Seconds(10), 1);
  controller_->OnAudioFramesConsumed(1024, base::Seconds(9), 1);
  EXPECT_EQ(controller_->ComputeAudioSampleAdjustment(0, 48000), 0);
  EXPECT_EQ(controller_->ComputeAudioSampleAdjustment(1024, 0), 0);
  EXPECT_EQ(controller_->ComputeAudioSampleAdjustment(-1, 48000), 0);
}

// ---- synchronize_audio_to_video port ---------------------------------------

TEST_F(AvSyncControllerTest, AlignIsIdentityWhenAudioIsNotTheMaster) {
  controller_->set_requested_master(M::kVideo);
  controller_->OnVideoFramePresented(base::Seconds(10), 1);
  controller_->OnAudioFramesConsumed(1024, base::Seconds(9), 1);
  EXPECT_EQ(controller_->AlignAudioDurationToVideo(base::Milliseconds(21)),
            base::Milliseconds(21));
}

TEST_F(AvSyncControllerTest, AlignShortensAudioWhenItIsBehindVideo) {
  controller_->OnVideoFramePresented(base::Seconds(10), 1);
  controller_->OnAudioFramesConsumed(1024, base::Seconds(9) + base::Milliseconds(900), 1);
  // Audio is 100 ms behind; shortening the next buffer lets it catch up.
  const base::TimeDelta aligned =
      controller_->AlignAudioDurationToVideo(base::Milliseconds(100));
  EXPECT_LT(aligned, base::Milliseconds(100));
  // Bounded by SAMPLE_CORRECTION_PERCENT_MAX (10%): at most 10 ms shorter.
  EXPECT_GE(aligned, base::Milliseconds(90));
}

TEST_F(AvSyncControllerTest, AlignLengthensAudioWhenItIsAheadOfVideo) {
  controller_->OnVideoFramePresented(base::Seconds(10), 1);
  controller_->OnAudioFramesConsumed(1024, base::Seconds(10) + base::Milliseconds(100), 1);
  const base::TimeDelta aligned =
      controller_->AlignAudioDurationToVideo(base::Milliseconds(100));
  EXPECT_GT(aligned, base::Milliseconds(100));
  EXPECT_LE(aligned, base::Milliseconds(110));
}

TEST_F(AvSyncControllerTest, AlignNeverReturnsNegative) {
  controller_->OnVideoFramePresented(base::Seconds(10), 1);
  controller_->OnAudioFramesConsumed(1024, base::Seconds(10) + base::Milliseconds(5), 1);
  const base::TimeDelta aligned =
      controller_->AlignAudioDurationToVideo(base::Microseconds(100));
  EXPECT_GE(aligned, base::TimeDelta());
}

TEST_F(AvSyncControllerTest, AlignIsIdentityBeyondNoSyncThreshold) {
  controller_->OnVideoFramePresented(base::Seconds(100), 1);
  controller_->OnAudioFramesConsumed(1024, base::Seconds(1), 1);
  EXPECT_EQ(controller_->AlignAudioDurationToVideo(base::Milliseconds(100)),
            base::Milliseconds(100));
}

// ---- TimeSource -------------------------------------------------------------

TEST_F(AvSyncControllerTest, WallClockTimesAreMonotonic) {
  controller_->OnAudioFramesConsumed(1024, base::Seconds(10), 1);
  const base::TimeTicks now = clock_.NowTicks();
  std::vector<WallClockTime> out;
  controller_->GetWallClockTimes(
      {base::Seconds(10), base::Seconds(11), base::Seconds(12)}, now, &out);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0].wall_time, now);                       // media == master
  EXPECT_EQ(out[1].wall_time - now, base::Seconds(1));
  EXPECT_EQ(out[2].wall_time - now, base::Seconds(2));
  EXPECT_LT(out[0].wall_time, out[1].wall_time);
  EXPECT_LT(out[1].wall_time, out[2].wall_time);
}

TEST_F(AvSyncControllerTest, WallClockTimesCompressAtDoubleRate) {
  controller_->OnAudioFramesConsumed(1024, base::Seconds(10), 1);
  controller_->SetPlaybackRate(2.0);
  const base::TimeTicks now = clock_.NowTicks();
  std::vector<WallClockTime> out;
  controller_->GetWallClockTimes({base::Seconds(10), base::Seconds(12)}, now, &out);
  ASSERT_EQ(out.size(), 2u);
  // 2 s of media at 2x occupies 1 s of wall time.
  EXPECT_EQ(out[1].wall_time - now, base::Milliseconds(1000));
}

TEST_F(AvSyncControllerTest, WallClockTimesFallBackWhenClockInvalid) {
  const base::TimeTicks now = clock_.NowTicks();
  std::vector<WallClockTime> out;
  controller_->GetWallClockTimes({base::Seconds(1)}, now, &out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].wall_time, now);
}

// ---- diagnostics ------------------------------------------------------------

TEST_F(AvSyncControllerTest, SnapshotCarriesEverything) {
  controller_->OnAudioFramesConsumed(1024, base::Seconds(5), 3);
  controller_->OnVideoFramePresented(base::Seconds(5) - base::Milliseconds(20), 3);
  const auto snapshot = controller_->GetSnapshot();
  EXPECT_TRUE(snapshot.audio.valid);
  EXPECT_TRUE(snapshot.video.valid);
  EXPECT_EQ(snapshot.audio.serial, 3);
  EXPECT_EQ(snapshot.requested, M::kAudio);
  EXPECT_EQ(snapshot.resolved, M::kAudio);
  EXPECT_EQ(snapshot.master, base::Seconds(5));
  EXPECT_EQ(snapshot.av_diff, base::Milliseconds(20));
  EXPECT_TRUE(snapshot.master_valid);
}

TEST_F(AvSyncControllerTest, MasterTypeNames) {
  EXPECT_STREQ(GetMasterTypeName(M::kAudio), "audio");
  EXPECT_STREQ(GetMasterTypeName(M::kVideo), "video");
  EXPECT_STREQ(GetMasterTypeName(M::kExternal), "external");
}

// ---- seqlock correctness (behaviour difference Δ14) -------------------------

// ffplay reads the clock's plain doubles from the video thread while the audio
// thread writes them. This test is the executable version of "that is a data
// race": under TSan it must be clean, and under any build the invariant
// pts == drift + (now - updated_at) must hold for every value a reader sees.
TEST_F(AvSyncControllerTest, ConcurrentReadersNeverSeeATornClock) {
  std::atomic<bool> stop{false};
  std::atomic<int> violations{0};
  std::atomic<int64_t> reads{0};

  std::thread writer([this, &stop]() {
    base::TimeDelta pts = base::Seconds(1);
    while (!stop.load(std::memory_order_relaxed)) {
      controller_->OnAudioFramesConsumed(1024, pts, 1);
      pts = pts + base::Milliseconds(21);
      if (pts > base::Seconds(600)) pts = base::Seconds(1);
    }
  });

  // Readers must not start until the clock has been set at least once. A read
  // that lands before the writer's first OnAudioFramesConsumed() sees an invalid
  // clock and GetMasterClock() returns kNoTimestamp -- which is correct
  // behaviour, not a torn read, but this test counts it as a violation. Without
  // this gate the test is a startup race: it passes when the writer thread wins
  // scheduling and fails with tens of thousands of "violations" when a reader
  // does. Observed as 33309 violations under `ctest --repeat until-fail`.
  for (int i = 0; i < 100000 && !controller_->master_clock_valid(); ++i) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(controller_->master_clock_valid())
      << "the writer never published a clock, so there is nothing to test";

  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([this, &stop, &violations, &reads]() {
      base::TimeDelta previous;
      bool have_previous = false;
      while (!stop.load(std::memory_order_relaxed)) {
        const base::TimeDelta now = controller_->GetMasterClock();
        reads.fetch_add(1, std::memory_order_relaxed);
        // The clock was valid before the readers started and the writer never
        // invalidates it, so kNoTimestamp here can only mean a torn read that
        // picked up valid_ == false between the seqlock's two halves.
        if (IsNoTimestamp(now)) {
          violations.fetch_add(1);
          continue;
        }
        // A torn read shows up as a value far outside the writer's range, or as
        // a large backwards jump. The writer only ever moves forward within
        // [1 s, 600 s].
        if (now < base::Milliseconds(900) || now > base::Seconds(601)) {
          violations.fetch_add(1);
        }
        // Monotonicity is deliberately NOT checked here: the writer wraps pts
        // back to 1 s when it passes 600 s, so a large backwards step is
        // expected. Ordering is covered by AvSyncControllerTest.MasterClock
        // AdvancesWithWallTime; what this test detects is a torn read, which
        // shows up as a value outside the writer's range.
        (void)have_previous;
        previous = now;
        have_previous = true;
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true);
  writer.join();
  for (auto& r : readers) r.join();

  EXPECT_EQ(violations.load(), 0);
  EXPECT_GT(reads.load(), 1000);
}

}  // namespace
}  // namespace ijkpp::media
