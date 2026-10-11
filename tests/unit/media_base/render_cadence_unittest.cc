// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/render_cadence.h"

#include "gtest/gtest.h"

namespace avbase::media {
namespace {

constexpr base::TimeDelta kInterval = base::Milliseconds(1000) / 60;

// Drives `frames` iterations of a render loop that spends `spent_ms` drawing
// and then wakes `jitter_ms` later than it asked to, and returns the average
// period it actually achieved. Pure arithmetic, so the cadence can be judged
// without a display, a runner, or a clock.
double AveragePeriod(double spent_ms, double jitter_ms, int frames) {
  const base::TimeTicks origin = base::TimeTicks::Now();
  base::TimeTicks now = origin;
  base::TimeTicks deadline;
  for (int i = 0; i < frames; ++i) {
    const base::TimeTicks frame_start = now;
    now += base::Milliseconds(spent_ms);
    const FramePacing pacing =
        NextFrameDelay(frame_start, now, deadline, kInterval);
    now += pacing.delay + base::Milliseconds(jitter_ms);
    deadline = pacing.next_deadline;
  }
  return (now - origin).InMillisecondsF() / frames;
}

TEST(NextFrameDelayTest, FirstFrameAnchorsOnItsOwnStart) {
  const base::TimeTicks start = base::TimeTicks::Now();
  const base::TimeTicks now = start + base::Milliseconds(2.0);
  const FramePacing pacing =
      NextFrameDelay(start, now, base::TimeTicks(), kInterval);
  EXPECT_NEAR(pacing.delay.InMillisecondsF(), 14.667, 0.01);
  EXPECT_EQ(pacing.next_deadline, start + kInterval);
}

// The bug this cadence exists to fix. Budgeting the next interval from "now"
// charges the wake-up overshoot to every period: 16.67 ms of interval plus
// ~1.8 ms of overshoot is 18.5 ms, which is 54 fps, and no single frame looks
// wrong enough to notice.
TEST(NextFrameDelayTest, WakeupOvershootDoesNotEnterThePeriod) {
  const double period = AveragePeriod(/*spent_ms=*/0.6, /*jitter_ms=*/1.8, 60);
  EXPECT_NEAR(period, 16.667, 0.5);
  // The regression bound that matters: the measuring-from-now version lands
  // at 18.47 ms here and fails this.
  EXPECT_LT(period, 17.5);
}

TEST(NextFrameDelayTest, AJitteryWakeupTradesAgainstTheNextSleep) {
  // One late frame is answered by a shorter sleep, not by a permanently
  // slower cadence.
  const base::TimeTicks start = base::TimeTicks::Now();
  const FramePacing anchored = NextFrameDelay(
      start, start + base::Milliseconds(1.0), base::TimeTicks(), kInterval);
  base::TimeTicks now = start + base::Milliseconds(1.0) + anchored.delay +
                        base::Milliseconds(3.0);  // woke 3 ms late
  const FramePacing next =
      NextFrameDelay(now, now, anchored.next_deadline, kInterval);
  EXPECT_NEAR(next.delay.InMillisecondsF(), 13.667, 0.01);
}

TEST(NextFrameDelayTest, APresentThatWaitedOnTheRefreshGoesStraightBack) {
  const base::TimeTicks start = base::TimeTicks::Now();
  const FramePacing pacing = NextFrameDelay(
      start, start + base::Milliseconds(10.0), base::TimeTicks(), kInterval);
  EXPECT_EQ(pacing.delay.InMillisecondsF(), 0.0);
  // And it drops the anchor: the refresh is the pacing source now, and keeping
  // a deadline from the timer regime would fight it.
  EXPECT_TRUE(pacing.next_deadline.is_null());
}

TEST(NextFrameDelayTest, LongStallDoesNotChaseTheDeadline) {
  const base::TimeTicks start = base::TimeTicks::Now();
  const FramePacing anchored = NextFrameDelay(
      start, start + base::Milliseconds(1.0), base::TimeTicks(), kInterval);
  // A seek / window drag / GC: half a second behind.
  const base::TimeTicks late =
      anchored.next_deadline + base::Milliseconds(500.0);
  const FramePacing pacing =
      NextFrameDelay(late, late, anchored.next_deadline, kInterval);
  EXPECT_EQ(pacing.delay.InMillisecondsF(), 0.0);
  EXPECT_EQ(pacing.next_deadline, late);
}

TEST(NextFrameDelayTest, NonPositiveIntervalIsNotACadence) {
  const base::TimeTicks start = base::TimeTicks::Now();
  const FramePacing pacing =
      NextFrameDelay(start, start + base::Milliseconds(1.0), base::TimeTicks(),
                     base::TimeDelta());
  EXPECT_EQ(pacing.delay.InMillisecondsF(), 0.0);
  EXPECT_TRUE(pacing.next_deadline.is_null());
}

}  // namespace
}  // namespace avbase::media
