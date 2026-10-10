// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/legacy/clock.h"

#include "base/time/simple_test_tick_clock.h"
#include "base/time/time.h"
#include "media/base/media_constants.h"
#include "gtest/gtest.h"

namespace avbase::media {
namespace {

// H1: speed 0 must freeze the clock at its anchor. The old `speed > 0 ?
// speed : 1.0` in Get() turned every pause into rate 1, so Get() kept running
// while the pipeline believed it was paused -- directly contradicting the
// "speed 0 keeps the anchor" contract in renderer_impl_controls.cc.
TEST(ClockTest, PausedClockFreezesAtAnchor) {
  base::SimpleTestTickClock wall;
  Clock clock(&wall);

  clock.Set(base::Seconds(10), /*serial=*/0);
  clock.SetSpeed(0.0f);
  wall.Advance(base::Seconds(5));
  EXPECT_EQ(clock.Get(), base::Seconds(10))
      << "a paused clock must not advance with the wall clock";
  wall.Advance(base::Seconds(5));
  EXPECT_EQ(clock.Get(), base::Seconds(10));
}

// Resuming from a pause continues from the frozen anchor: the paused wall
// time itself must NOT be consumed as media time.
TEST(ClockTest, ResumeAfterPauseContinuesFromAnchor) {
  base::SimpleTestTickClock wall;
  Clock clock(&wall);

  clock.Set(base::Seconds(10), /*serial=*/0);
  clock.SetSpeed(0.0f);
  wall.Advance(base::Seconds(30));  // Paused for 30 wall seconds.
  clock.SetSpeed(1.0f);             // Resume.
  wall.Advance(base::Seconds(2));
  EXPECT_EQ(clock.Get(), base::Seconds(12))
      << "media time must resume from the anchor, not include the pause";
}

// Changing speed mid-play must be continuous at the instant of the change:
// the elapsed interval that was already played at the old rate keeps the old
// rate. The old re-anchor replayed the whole interval at rate 1, jumping the
// clock BACKWARDS when slowing down from 2x.
TEST(ClockTest, SpeedChangeFromTwoTimesToNormalIsContinuous) {
  base::SimpleTestTickClock wall;
  Clock clock(&wall);

  clock.Set(base::Seconds(10), /*serial=*/0);
  clock.SetSpeed(2.0f);
  wall.Advance(base::Seconds(4));
  EXPECT_EQ(clock.Get(), base::Seconds(18));  // 10 + 4 * 2

  clock.SetSpeed(1.0f);  // Re-anchor must happen at 18s, not at 14s.
  wall.Advance(base::Seconds(2));
  EXPECT_EQ(clock.Get(), base::Seconds(20))
      << "slowing down must not rewind media time";
}

// Symmetric direction: speeding up from 0.5x must not jump forward.
TEST(ClockTest, SpeedChangeFromHalfTimesToNormalIsContinuous) {
  base::SimpleTestTickClock wall;
  Clock clock(&wall);

  clock.Set(base::Seconds(10), /*serial=*/0);
  clock.SetSpeed(0.5f);
  wall.Advance(base::Seconds(4));
  EXPECT_EQ(clock.Get(), base::Seconds(12));  // 10 + 4 * 0.5

  clock.SetSpeed(1.0f);  // Re-anchor must happen at 12s, not at 14s.
  wall.Advance(base::Seconds(2));
  EXPECT_EQ(clock.Get(), base::Seconds(14))
      << "speeding up must not fast-forward media time";
}

// Sanity: the plain 1x extrapolation is unchanged by the fix.
TEST(ClockTest, NormalPlaybackExtrapolates) {
  base::SimpleTestTickClock wall;
  Clock clock(&wall);

  clock.Set(base::Seconds(10), /*serial=*/0);
  wall.Advance(base::Seconds(3));
  EXPECT_EQ(clock.Get(), base::Seconds(13));

  clock.SetTo(base::Seconds(20));
  wall.Advance(base::Seconds(1));
  EXPECT_EQ(clock.Get(), base::Seconds(21));
}

TEST(ClockTest, InvalidClockStaysInvalid) {
  base::SimpleTestTickClock wall;
  Clock clock(&wall);
  EXPECT_EQ(clock.Get(), media::kNoTimestamp);
  clock.Invalidate();
  EXPECT_EQ(clock.Get(), media::kNoTimestamp);
}

}  // namespace
}  // namespace avbase::media
