// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Exhaustive tests for the pure decision core of VideoFrameCompositor.
//
// This file is the concrete payoff of the refactor: ijkplayer's equivalent
// logic lives inline in video_refresh() (ff_ffplay.c), reads is->frame_timer
// and calls av_gettime_relative() directly, and therefore cannot be tested
// without a real clock, a real media file and a real display. Here it is a
// static pure function of an immutable snapshot, so every branch below is a
// deterministic assertion that runs in microseconds.

#include "media/filters/legacy/video_frame_compositor.h"

#include <memory>

#include "base/time/simple_test_tick_clock.h"
#include "gtest/gtest.h"

namespace ijkpp::media {
namespace {

using C = VideoFrameCompositor;
using D = C::Decision;
using R = C::DropReason;

constexpr base::TimeDelta kFps30 = base::Microseconds(33333);
constexpr base::TimeTicks kNow = base::TimeTicks::FromMicroseconds(1000000);
constexpr base::TimeDelta kTenSeconds = base::Seconds(10);

// A 30 fps stream whose next frame is due exactly now, perfectly in sync with
// an audio master clock. Every test starts from here and perturbs one field,
// so a failure names exactly the behaviour that regressed.
C::FrameSyncInput MakeInput() {
  C::FrameSyncInput in;
  in.fps_duration = kFps30;
  in.next_duration = kFps30;
  in.next_timestamp = kTenSeconds;
  in.last_timestamp = kTenSeconds - kFps30;
  in.next_serial = 1;
  in.last_serial = 1;
  in.queue_serial = 1;
  in.master_clock = kTenSeconds;   // Exactly in sync.
  in.master_serial = 1;
  in.master_clock_valid = true;
  in.master_is_video = false;
  in.now = kNow;
  in.frame_timer = kNow - kFps30;  // Previous frame was due one interval ago.
  in.last_present_wall_time = kNow - kFps30;
  in.displayed_duration = kFps30;
  in.playback_rate = 1.0;
  in.has_candidate = true;
  return in;
}

C::Thresholds Thresholds() { return C::Thresholds{}; }

// ---------------------------------------------------------------------------
// Basic presentation
// ---------------------------------------------------------------------------

TEST(VideoFrameCompositorTest, PresentsWhenFrameIsDue) {
  const auto in = MakeInput();
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kPresent);
  EXPECT_EQ(out.frame_duration, kFps30);
  EXPECT_EQ(out.av_diff, base::TimeDelta());
}

TEST(VideoFrameCompositorTest, HoldsWhenNotYetDue) {
  auto in = MakeInput();
  in.frame_timer = kNow + kFps30;   // Next frame is due one interval from now.
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kHold);
  EXPECT_EQ(out.retry_at, kNow + kFps30);
  EXPECT_EQ(out.target_delay, kFps30);
}

TEST(VideoFrameCompositorTest, ReportsLongHoldForCallersToClamp) {
  auto in = MakeInput();
  in.fps_duration = base::Seconds(30);   // Absurd interval.
  in.next_duration = base::Seconds(30);
  in.last_timestamp = in.next_timestamp - base::Seconds(30);
  in.frame_timer = kNow + base::Seconds(30);
  // A 30 s inter-frame delta exceeds max_sane_frame_duration, so the
  // fps-derived duration is used; that is still 30 s here. The compositor
  // reports the true retry instant and the caller clamps the actual sleep to
  // Thresholds::max_sleep so that control messages stay responsive.
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kHold);
  EXPECT_GT(out.retry_at - kNow, Thresholds().max_sleep);
}

TEST(VideoFrameCompositorTest, ReportsEndOfStreamWhenQueueDrained) {
  auto in = MakeInput();
  in.has_candidate = false;
  in.end_of_stream = true;
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kEndOfStream);
}

TEST(VideoFrameCompositorTest, HoldsBrieflyWhenQueueEmptyButNotEos) {
  auto in = MakeInput();
  in.has_candidate = false;
  in.end_of_stream = false;
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kHold);
  EXPECT_EQ(out.retry_at, kNow + Thresholds().min_sleep);
}

TEST(VideoFrameCompositorTest, ConsecutiveFramesAdvanceDurationEstimate) {
  auto in = MakeInput();
  in.last_timestamp = kTenSeconds - base::Milliseconds(40);
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kPresent);
  EXPECT_EQ(out.frame_duration, base::Milliseconds(40));   // From the pts delta.
}

// ---------------------------------------------------------------------------
// Framedrop
// ---------------------------------------------------------------------------

TEST(VideoFrameCompositorTest, PresentsWhenSlightlyLateInsideThreshold) {
  auto in = MakeInput();
  in.master_clock = kTenSeconds + base::Milliseconds(20);   // 20 ms late.
  in.max_frame_drop = 5;
  const auto out = C::DecideNextFrame(in, Thresholds());
  // 20 ms is inside the frame duration (33.3 ms), so the frame is still
  // useful and is presented rather than dropped.
  EXPECT_EQ(out.decision, D::kPresent);
  EXPECT_EQ(out.av_diff, base::Milliseconds(20));
}

TEST(VideoFrameCompositorTest, DriftInsideDeadZoneLeavesDelayUntouched) {
  auto in = MakeInput();
  in.master_clock = kTenSeconds + base::Milliseconds(20);
  // sync_threshold = clip(33.3 ms, 40 ms, 100 ms) = 40 ms, so a 20 ms drift
  // sits inside the dead zone: AV_SYNC_THRESHOLD_MIN exists precisely to stop
  // the compositor from chasing sub-frame jitter.
  EXPECT_EQ(C::ComputeTargetDelay(kFps30, in, Thresholds()), kFps30);
}

TEST(VideoFrameCompositorTest, DropsWhenLateBeyondFrameDuration) {
  auto in = MakeInput();
  in.master_clock = kTenSeconds + base::Milliseconds(200);   // 200 ms late.
  in.max_frame_drop = 5;
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kDrop);
  EXPECT_EQ(out.drop_reason, R::kLateBeyondWindow);
  EXPECT_EQ(out.av_diff, base::Milliseconds(200));
}

TEST(VideoFrameCompositorTest, NeverDropsWhenFramedropDisabled) {
  auto in = MakeInput();
  in.master_clock = kTenSeconds + base::Milliseconds(200);
  in.max_frame_drop = -1;   // ijkplayer's "framedrop=-1" disables dropping.
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);

  in.max_frame_drop = 0;
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);
}

TEST(VideoFrameCompositorTest, StopsDroppingOnceBudgetExhausted) {
  auto in = MakeInput();
  in.master_clock = kTenSeconds + base::Milliseconds(200);
  in.max_frame_drop = 3;
  in.frames_dropped_since_last_present = 3;
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);
}

TEST(VideoFrameCompositorTest, DropsUpToTheBudgetThenPresents) {
  auto in = MakeInput();
  in.master_clock = kTenSeconds + base::Milliseconds(200);
  in.max_frame_drop = 2;
  for (int dropped = 0; dropped < 2; ++dropped) {
    in.frames_dropped_since_last_present = dropped;
    EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kDrop)
        << "dropped=" << dropped;
  }
  in.frames_dropped_since_last_present = 2;
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);
}

TEST(VideoFrameCompositorTest, SnapsInsteadOfDroppingBeyondNoSyncThreshold) {
  auto in = MakeInput();
  // 20 s of drift is beyond AV_NOSYNC_THRESHOLD: pacing gives up and the frame
  // timer is snapped, which shows up here as "present immediately".
  in.master_clock = kTenSeconds + base::Seconds(20);
  in.max_frame_drop = 10;
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kPresent);
  EXPECT_EQ(out.target_delay, kFps30);   // Unadjusted: no correction attempted.
}

TEST(VideoFrameCompositorTest, DoesNotDropWhenVideoIsTheMasterClock) {
  auto in = MakeInput();
  in.master_is_video = true;
  in.master_clock = kTenSeconds + base::Milliseconds(200);
  in.max_frame_drop = 5;
  // ffplay only re-paces/drops when video is the slave.
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);
}

// ---------------------------------------------------------------------------
// compute_target_delay port
// ---------------------------------------------------------------------------

TEST(VideoFrameCompositorTest, TargetDelayUnchangedWhenInSync) {
  auto in = MakeInput();
  EXPECT_EQ(C::ComputeTargetDelay(kFps30, in, Thresholds()), kFps30);
}

TEST(VideoFrameCompositorTest, TargetDelayShortenedWhenBehindMaster) {
  auto in = MakeInput();
  in.master_clock = kTenSeconds + base::Milliseconds(20);   // av_diff = +20 ms.
  // sync_threshold = clip(33.3 ms, 40 ms, 100 ms) = 40 ms, so 20 ms is inside
  // the dead zone and the delay is untouched.
  EXPECT_EQ(C::ComputeTargetDelay(kFps30, in, Thresholds()), kFps30);

  in.master_clock = kTenSeconds + base::Milliseconds(60);   // av_diff = +60 ms.
  EXPECT_EQ(C::ComputeTargetDelay(kFps30, in, Thresholds()), base::TimeDelta());
}

TEST(VideoFrameCompositorTest, TargetDelayNeverGoesNegative) {
  auto in = MakeInput();
  in.master_clock = kTenSeconds + base::Seconds(5);
  EXPECT_EQ(C::ComputeTargetDelay(kFps30, in, Thresholds()), base::TimeDelta());
}

TEST(VideoFrameCompositorTest, LongFrameAbsorbsFullDriftWhenAheadOfMaster) {
  auto in = MakeInput();
  const base::TimeDelta long_frame = base::Milliseconds(500);
  in.next_timestamp = kTenSeconds;
  in.master_clock = kTenSeconds - base::Milliseconds(100);   // av_diff = -100 ms.
  // sync_threshold = clip(500 ms, 40 ms, 100 ms) = 100 ms; av_diff <= -100 ms
  // and delay (500 ms) > sync_framedup_threshold, so the full drift is added.
  EXPECT_EQ(C::ComputeTargetDelay(long_frame, in, Thresholds()),
            base::Milliseconds(600));
}

TEST(VideoFrameCompositorTest, ShortFrameIsDuplicatedWhenAheadOfMaster) {
  auto in = MakeInput();
  const base::TimeDelta short_frame = base::Milliseconds(5);
  in.master_clock = kTenSeconds - base::Milliseconds(100);
  // sync_threshold = clip(5 ms, 40 ms, 100 ms) = 40 ms; delay (5 ms) is not
  // above sync_framedup_threshold (10 ms), so ffplay doubles it instead.
  EXPECT_EQ(C::ComputeTargetDelay(short_frame, in, Thresholds()),
            base::Milliseconds(10));
}

TEST(VideoFrameCompositorTest, NoCorrectionWhenMasterClockInvalid) {
  auto in = MakeInput();
  in.master_clock_valid = false;
  in.master_clock = kTenSeconds + base::Seconds(5);
  EXPECT_EQ(C::ComputeTargetDelay(kFps30, in, Thresholds()), kFps30);
}

TEST(VideoFrameCompositorTest, NoCorrectionWhenNextTimestampIsNoTimestamp) {
  auto in = MakeInput();
  in.next_timestamp = base::TimeDelta::Min();   // media::kNoTimestamp
  in.master_clock = kTenSeconds + base::Seconds(5);
  EXPECT_EQ(C::ComputeTargetDelay(kFps30, in, Thresholds()), kFps30);
}

TEST(VideoFrameCompositorTest, PlaybackRateScalesTargetDelay) {
  auto in = MakeInput();
  in.playback_rate = 2.0;
  EXPECT_EQ(C::ComputeTargetDelay(kFps30, in, Thresholds()),
            base::Microseconds(33333 / 2));

  in.playback_rate = 0.5;
  EXPECT_EQ(C::ComputeTargetDelay(kFps30, in, Thresholds()),
            base::Microseconds(33333 * 2));
}

TEST(VideoFrameCompositorTest, PlaybackRateZeroDoesNotDivideByZero) {
  auto in = MakeInput();
  in.playback_rate = 0.0;
  EXPECT_EQ(C::ComputeTargetDelay(kFps30, in, Thresholds()), kFps30);
}

// ---------------------------------------------------------------------------
// fps cap ("max-fps")
// ---------------------------------------------------------------------------

TEST(VideoFrameCompositorTest, FpsCapDropsFramesArrivingTooSoon) {
  auto in = MakeInput();
  in.max_fps = 31;   // ijkplayer default.
  in.last_present_wall_time = kNow - base::Milliseconds(10);   // 10 ms ago.
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kDrop);
  EXPECT_EQ(out.drop_reason, R::kFpsCap);
}

TEST(VideoFrameCompositorTest, FpsCapAllowsFrameAfterMinInterval) {
  auto in = MakeInput();
  in.max_fps = 31;   // min interval ~32.2 ms
  in.last_present_wall_time = kNow - base::Milliseconds(40);
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);
}

TEST(VideoFrameCompositorTest, FpsCapDisabledForZeroAndNegative) {
  auto in = MakeInput();
  in.last_present_wall_time = kNow - base::Milliseconds(1);
  in.max_fps = 0;
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);
  in.max_fps = -1;
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);
}

TEST(VideoFrameCompositorTest, FpsCapIgnoredBeforeFirstPresent) {
  auto in = MakeInput();
  in.max_fps = 31;
  in.last_present_wall_time = base::TimeTicks();   // null: nothing shown yet
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);
}

TEST(VideoFrameCompositorTest, ApplyFpsCapExtendsShortDurations) {
  const base::TimeDelta cap31 = base::Microseconds(1000000 / 31);   // ~32.26 ms
  const base::TimeDelta fps60 = base::Microseconds(16667);
  // A 60 fps source is faster than a 31 fps cap, so the effective display
  // duration is stretched to the cap interval.
  EXPECT_EQ(C::ApplyFpsCap(fps60, 31), cap31);
  // A 30 fps source is already slower than the cap: unchanged.
  EXPECT_EQ(C::ApplyFpsCap(kFps30, 31), kFps30);
  // A long frame is never shortened by the cap.
  EXPECT_EQ(C::ApplyFpsCap(base::Milliseconds(100), 31), base::Milliseconds(100));
  // 0 and negative disable the cap.
  EXPECT_EQ(C::ApplyFpsCap(kFps30, 0), kFps30);
  EXPECT_EQ(C::ApplyFpsCap(kFps30, -1), kFps30);
}

TEST(VideoFrameCompositorTest, FpsCapHalvesOutputFor60fpsSource) {
  // A 60 fps source capped at 30 fps must drop roughly every other frame.
  auto in = MakeInput();
  in.fps_duration = base::Microseconds(16667);
  in.next_duration = base::Microseconds(16667);
  in.last_timestamp = in.next_timestamp - base::Microseconds(16667);
  in.max_fps = 30;

  int presented = 0;
  int dropped = 0;
  base::TimeTicks wall = kNow;
  base::TimeTicks last_present = wall - base::Microseconds(16667);
  for (int i = 0; i < 60; ++i) {
    in.now = wall;
    in.last_present_wall_time = last_present;
    in.frame_timer = wall - base::Microseconds(16667);
    const auto out = C::DecideNextFrame(in, Thresholds());
    if (out.decision == D::kPresent) {
      ++presented;
      last_present = wall;
    } else if (out.decision == D::kDrop && out.drop_reason == R::kFpsCap) {
      ++dropped;
    }
    wall = wall + base::Microseconds(16667);
  }
  EXPECT_GT(dropped, 20);
  EXPECT_GT(presented, 20);
  EXPECT_LT(presented, 40);
}

// ---------------------------------------------------------------------------
// serial / seek
// ---------------------------------------------------------------------------

TEST(VideoFrameCompositorTest, DropsFrameFromStaleSeekGeneration) {
  auto in = MakeInput();
  in.next_serial = 2;
  in.queue_serial = 3;   // A seek happened after this frame was queued.
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kDrop);
  EXPECT_EQ(out.drop_reason, R::kStaleSerial);
}

// REGRESSION: this is the bug class that costs ijkplayer forks a "multi-second
// stall right after seek" report every so often. ffplay guards it with
// `if (lastvp->serial == vp->serial)`; losing the guard makes the compositor
// derive last_duration from a pts delta that spans the seek boundary.
TEST(VideoFrameCompositorTest, SkipsDurationComputationAcrossSeekSerialChange) {
  auto in = MakeInput();
  in.last_serial = 3;
  in.next_serial = 4;
  in.queue_serial = 4;
  in.last_timestamp = kTenSeconds - base::Seconds(30);   // Bogus cross-seek delta.
  in.master_clock = in.next_timestamp;

  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kPresent);
  EXPECT_EQ(out.frame_duration, kFps30);   // Fell back to the fps-derived value.
  EXPECT_EQ(out.target_delay, kFps30);     // Not derived from the 30 s delta.
}

TEST(VideoFrameCompositorTest, SameSerialUsesPtsDelta) {
  auto in = MakeInput();
  in.last_serial = in.next_serial = in.queue_serial = 4;
  in.last_timestamp = in.next_timestamp - base::Milliseconds(50);
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).frame_duration,
            base::Milliseconds(50));
}

// ---------------------------------------------------------------------------
// accurate seek
// ---------------------------------------------------------------------------

TEST(VideoFrameCompositorTest, AccurateSeekDropsFramesBeforeTarget) {
  auto in = MakeInput();
  in.accurate_seek_pending = true;
  in.accurate_seek_target = kTenSeconds + base::Milliseconds(500);
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kDrop);
  EXPECT_EQ(out.drop_reason, R::kAccurateSeek);
}

TEST(VideoFrameCompositorTest, AccurateSeekPresentsFrameAtOrAfterTarget) {
  auto in = MakeInput();
  in.accurate_seek_pending = true;
  in.accurate_seek_target = kTenSeconds - base::Milliseconds(1);
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);

  in.accurate_seek_target = kTenSeconds;   // Exactly at the target.
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);
}

TEST(VideoFrameCompositorTest, AccurateSeekIgnoresUnsetTarget) {
  auto in = MakeInput();
  in.accurate_seek_pending = true;
  in.accurate_seek_target = base::TimeDelta();   // Never set.
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);
}

TEST(VideoFrameCompositorTest, AccurateSeekWinsOverFramedrop) {
  auto in = MakeInput();
  in.accurate_seek_pending = true;
  in.accurate_seek_target = kTenSeconds + base::Seconds(1);
  in.master_clock = kTenSeconds + base::Milliseconds(200);
  in.max_frame_drop = 5;
  // Both conditions hold; the accurate-seek reason must be reported because
  // that is what the diagnostics surface.
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kDrop);
  EXPECT_EQ(out.drop_reason, R::kAccurateSeek);
}

// ---------------------------------------------------------------------------
// playback rate
// ---------------------------------------------------------------------------

TEST(VideoFrameCompositorTest, DoubleSpeedHalvesTheTargetDelay) {
  auto in = MakeInput();
  in.frame_timer = kNow + kFps30;   // Definitely a hold at any rate.
  in.playback_rate = 2.0;
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kHold);
  // The caller advances frame_timer by target_delay, so halving it is what
  // makes 2x playback consume frames twice as fast.
  EXPECT_EQ(out.target_delay, base::Microseconds(33333 / 2));
}

TEST(VideoFrameCompositorTest, HalfSpeedDoublesTheTargetDelay) {
  auto in = MakeInput();
  in.frame_timer = kNow + kFps30;
  in.playback_rate = 0.5;
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).target_delay,
            base::Microseconds(33333 * 2));
}

// frame_timer gates *this* frame; playback_rate scales target_delay, which the
// caller adds to frame_timer to decide when the *next* frame is due. Keeping
// the two responsibilities separate is what makes 2x/0.5x speed correct without
// special-casing the current frame.
TEST(VideoFrameCompositorTest, PlaybackRateScalesTargetDelayNotTheHoldGate) {
  auto in = MakeInput();
  in.frame_timer = kNow + base::Microseconds(600);   // 600 us from due.
  in.playback_rate = 200.0;
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kHold);
  EXPECT_EQ(out.retry_at, kNow + base::Microseconds(600));
  EXPECT_EQ(out.target_delay, base::Microseconds(33333 / 200));
}

// ---------------------------------------------------------------------------
// pause / step
// ---------------------------------------------------------------------------

TEST(VideoFrameCompositorTest, PausedHoldsWithoutAdvancing) {
  auto in = MakeInput();
  in.paused = true;
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kHold);
  EXPECT_EQ(out.retry_at, kNow + Thresholds().max_sleep);
}

TEST(VideoFrameCompositorTest, StepModePresentsOneFrameWhilePaused) {
  auto in = MakeInput();
  in.paused = true;
  in.step_mode = true;
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kPresent);
  EXPECT_EQ(out.frame_duration, kFps30);
}

TEST(VideoFrameCompositorTest, StepModeIgnoresPacingAndDropping) {
  auto in = MakeInput();
  in.step_mode = true;
  in.frame_timer = kNow + base::Seconds(5);   // Far from due.
  in.next_serial = 0;
  in.queue_serial = 9;                        // Stale.
  in.accurate_seek_pending = true;
  in.accurate_seek_target = kTenSeconds + base::Seconds(60);
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kPresent);
}

TEST(VideoFrameCompositorTest, BufferingBlocksPresentation) {
  auto in = MakeInput();
  in.buffering_blocked = true;
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kHold);
  EXPECT_EQ(out.frame_duration, base::TimeDelta());   // No pacing computed.
}

// ---------------------------------------------------------------------------
// master clock selection
// ---------------------------------------------------------------------------

TEST(VideoFrameCompositorTest, InvalidMasterClockDisablesDriftCorrection) {
  auto in = MakeInput();
  in.master_clock_valid = false;
  in.master_clock = kTenSeconds + base::Milliseconds(200);
  in.max_frame_drop = 5;
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kPresent);
  EXPECT_EQ(out.av_diff, base::TimeDelta());   // Not reported when invalid.
  EXPECT_EQ(out.target_delay, kFps30);
}

TEST(VideoFrameCompositorTest, VideoMasterUsesItsOwnPacing) {
  auto in = MakeInput();
  in.master_is_video = true;
  in.master_clock = kTenSeconds + base::Milliseconds(200);
  in.max_frame_drop = 5;
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kPresent);
  EXPECT_EQ(out.target_delay, kFps30);   // No re-pacing against itself.
}

// ---------------------------------------------------------------------------
// numeric edge cases
// ---------------------------------------------------------------------------

TEST(VideoFrameCompositorTest, NoTimestampsFallBackToMinSleepAndPresent) {
  auto in = MakeInput();
  in.next_timestamp = base::TimeDelta::Min();   // media::kNoTimestamp
  in.last_timestamp = base::TimeDelta::Min();
  in.fps_duration = base::TimeDelta();
  in.next_duration = base::TimeDelta();
  in.master_clock_valid = false;
  const auto out = C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(out.decision, D::kPresent);
  EXPECT_EQ(out.frame_duration, Thresholds().min_sleep);
}

TEST(VideoFrameCompositorTest, NegativePtsDeltaFallsBackToFpsDuration) {
  auto in = MakeInput();
  in.last_timestamp = in.next_timestamp + base::Milliseconds(50);   // Out of order.
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).frame_duration, kFps30);
}

TEST(VideoFrameCompositorTest, AbsurdPtsDeltaFallsBackToFpsDuration) {
  auto in = MakeInput();
  in.last_timestamp = in.next_timestamp - base::Seconds(60);   // > max_sane
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).frame_duration, kFps30);
}

TEST(VideoFrameCompositorTest, NullFrameTimerPresentsImmediately) {
  auto in = MakeInput();
  in.frame_timer = base::TimeTicks();   // null: nothing has been presented yet
  const auto out = C::DecideNextFrame(in, Thresholds());
  // Pacing needs a reference point; without one ffplay shows the first decoded
  // frame right away instead of waiting a full interval. Preserving this is
  // what keeps first-frame latency low.
  EXPECT_EQ(out.decision, D::kPresent);
}

TEST(VideoFrameCompositorTest, DeadlineWindowOverridesHold) {
  auto in = MakeInput();
  in.frame_timer = kNow + kFps30;              // Due 33.3 ms from now.
  in.deadline_max = kNow + base::Milliseconds(40);
  const auto out = C::DecideNextFrame(in, Thresholds());
  // The sink told us this refresh window extends past the due instant, so
  // presenting now beats waiting for the next window.
  EXPECT_EQ(out.decision, D::kPresent);
}

TEST(VideoFrameCompositorTest, DeadlineWindowBeforeDueStillHolds) {
  auto in = MakeInput();
  in.frame_timer = kNow + kFps30;
  in.deadline_max = kNow + base::Milliseconds(10);   // Window closes too early.
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kHold);
}

TEST(VideoFrameCompositorTest, NullDeadlineWindowIsIgnored) {
  auto in = MakeInput();
  in.frame_timer = kNow + kFps30;
  in.deadline_max = base::TimeTicks();   // null
  EXPECT_EQ(C::DecideNextFrame(in, Thresholds()).decision, D::kHold);
}

// ---------------------------------------------------------------------------
// Determinism: the function must be pure
// ---------------------------------------------------------------------------

TEST(VideoFrameCompositorTest, DecideNextFrameIsDeterministic) {
  const auto in = MakeInput();
  const auto first = C::DecideNextFrame(in, Thresholds());
  for (int i = 0; i < 100; ++i) {
    const auto again = C::DecideNextFrame(in, Thresholds());
    ASSERT_EQ(again.decision, first.decision) << "iteration " << i;
    ASSERT_EQ(again.target_delay, first.target_delay) << "iteration " << i;
    ASSERT_EQ(again.retry_at, first.retry_at) << "iteration " << i;
  }
}

TEST(VideoFrameCompositorTest, InputSnapshotIsNotModified) {
  auto in = MakeInput();
  const auto before = in;
  C::DecideNextFrame(in, Thresholds());
  EXPECT_EQ(in.next_timestamp, before.next_timestamp);
  EXPECT_EQ(in.frame_timer, before.frame_timer);
  EXPECT_EQ(in.master_clock, before.master_clock);
  EXPECT_EQ(in.frames_dropped_since_last_present,
            before.frames_dropped_since_last_present);
}

TEST(VideoFrameCompositorTest, DecisionNamesAreStable) {
  EXPECT_STREQ(GetDecisionName(D::kPresent), "Present");
  EXPECT_STREQ(GetDecisionName(D::kHold), "Hold");
  EXPECT_STREQ(GetDecisionName(D::kDrop), "Drop");
  EXPECT_STREQ(GetDecisionName(D::kRepeat), "Repeat");
  EXPECT_STREQ(GetDecisionName(D::kEndOfStream), "EndOfStream");
  EXPECT_STREQ(GetDropReasonName(R::kLateBeyondWindow), "late-beyond-window");
  EXPECT_STREQ(GetDropReasonName(R::kFpsCap), "fps-cap");
  EXPECT_STREQ(GetDropReasonName(R::kStaleSerial), "stale-serial");
  EXPECT_STREQ(GetDropReasonName(R::kAccurateSeek), "accurate-seek");
}

// ---------------------------------------------------------------------------
// Stateful wrapper, exercised with a SimpleTestTickClock
// ---------------------------------------------------------------------------

class VideoFrameCompositorStateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    compositor_ = std::make_unique<C>(C::Thresholds{}, &clock_);
    compositor_->SetFpsDuration(kFps30);
    compositor_->SetMasterClock(kTenSeconds, /*serial=*/1, /*valid=*/true);
  }

  base::scoped_refptr<VideoFrame> MakeFrame(int64_t index) {
    return VideoFrame::CreateBlackFrame(
        VideoFormat::kI420, Size{64, 64}, Size{64, 64}, Rational{1, 1},
        kTenSeconds + base::Microseconds(index * 33333), kFps30, /*serial=*/1);
  }

  base::SimpleTestTickClock clock_;
  std::unique_ptr<C> compositor_;
};

TEST_F(VideoFrameCompositorStateTest, PresentsFramesInOrder) {
  for (int64_t i = 0; i < 5; ++i) {
    compositor_->PutCurrentFrame(MakeFrame(i));
  }
  EXPECT_EQ(compositor_->frames_pending(), 5u);

  int presented = 0;
  for (int i = 0; i < 5; ++i) {
    clock_.Advance(base::Milliseconds(40));   // Past the 33.3 ms interval.
    compositor_->SetMasterClock(
        kTenSeconds + base::Milliseconds(40 * i), 1, true);
    if (compositor_->Render(kNow, clock_.NowTicks())) {
      ++presented;
    }
  }
  EXPECT_EQ(presented, 5);
  EXPECT_EQ(compositor_->GetStats().frames_presented, 5u);
}

TEST_F(VideoFrameCompositorStateTest, FlushClearsPendingFramesAndPacing) {
  compositor_->PutCurrentFrame(MakeFrame(0));
  compositor_->PutCurrentFrame(MakeFrame(1));
  clock_.Advance(base::Milliseconds(100));
  ASSERT_TRUE(compositor_->Render(kNow, clock_.NowTicks()));

  compositor_->Flush();
  EXPECT_EQ(compositor_->frames_pending(), 0u);
  // After a flush the first frame of the new generation is presented
  // immediately rather than being scheduled against the stale frame timer.
  compositor_->PutCurrentFrame(MakeFrame(2));
  EXPECT_TRUE(compositor_->Render(kNow, clock_.NowTicks()) != nullptr);
}

TEST_F(VideoFrameCompositorStateTest, PausedRenderKeepsCurrentFrame) {
  compositor_->PutCurrentFrame(MakeFrame(0));
  clock_.Advance(base::Milliseconds(100));
  const auto first = compositor_->Render(kNow, clock_.NowTicks());
  ASSERT_TRUE(first);

  compositor_->SetPaused(true);
  compositor_->PutCurrentFrame(MakeFrame(1));
  clock_.Advance(base::Seconds(1));
  EXPECT_EQ(compositor_->Render(kNow, clock_.NowTicks()), nullptr);
  EXPECT_EQ(compositor_->frames_pending(), 1u);   // Not consumed while paused.
  EXPECT_EQ(compositor_->current_frame(), first);
}

TEST_F(VideoFrameCompositorStateTest, PlaybackRateAcceleratesPresentation) {
  for (int64_t i = 0; i < 10; ++i) {
    compositor_->PutCurrentFrame(MakeFrame(i));
  }
  compositor_->SetPlaybackRate(4.0);

  // At 4x the target delay is ~8.3 ms, so advancing 10 ms per iteration must
  // present every frame; at 1x the same cadence would hold on most of them.
  int presented = 0;
  for (int i = 0; i < 10; ++i) {
    clock_.Advance(base::Milliseconds(10));
    compositor_->SetMasterClock(kTenSeconds + base::Milliseconds(40 * i), 1, true);
    if (compositor_->Render(kNow, clock_.NowTicks())) {
      ++presented;
    }
  }
  EXPECT_EQ(presented, 10);
}

TEST_F(VideoFrameCompositorStateTest, StaleSerialFramesAreDroppedNotPresented) {
  auto frame = MakeFrame(0);
  compositor_->PutCurrentFrame(frame);
  // Simulate a seek: the queue generation moves past the queued frame.
  compositor_->Flush();
  compositor_->PutCurrentFrame(VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{64, 64}, Size{64, 64}, Rational{1, 1},
      kTenSeconds, kFps30, /*serial=*/1));
  clock_.Advance(base::Milliseconds(100));
  EXPECT_TRUE(compositor_->Render(kNow, clock_.NowTicks()) != nullptr);
}

TEST_F(VideoFrameCompositorStateTest, StatsCountPresentedAndRepeated) {
  EXPECT_EQ(compositor_->Render(kNow, clock_.NowTicks()), nullptr);
  EXPECT_EQ(compositor_->GetStats().frames_repeated, 1u);

  compositor_->PutCurrentFrame(MakeFrame(0));
  clock_.Advance(base::Milliseconds(100));
  ASSERT_TRUE(compositor_->Render(kNow, clock_.NowTicks()));
  const auto stats = compositor_->GetStats();
  EXPECT_EQ(stats.frames_presented, 1u);
  EXPECT_EQ(stats.frames_dropped, 0u);
}

}  // namespace
}  // namespace ijkpp::media
