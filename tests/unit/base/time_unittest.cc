// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/time/time.h"

#include "base/time/simple_test_tick_clock.h"
#include "gtest/gtest.h"

namespace ijkpp::base {
namespace {

TEST(TimeDeltaTest, DefaultIsZero) {
  EXPECT_TRUE(TimeDelta().is_zero());
  EXPECT_EQ(TimeDelta().InMicroseconds(), 0);
}

TEST(TimeDeltaTest, FactoriesUseExplicitUnits) {
  EXPECT_EQ(Microseconds(1).InMicroseconds(), 1);
  EXPECT_EQ(Milliseconds(1).InMicroseconds(), 1000);
  EXPECT_EQ(Seconds(1).InMicroseconds(), 1000000);
  EXPECT_EQ(Minutes(1).InMicroseconds(), 60000000);
  EXPECT_EQ(Seconds(2).InMilliseconds(), 2000);
  EXPECT_EQ(Milliseconds(1500).InSeconds(), 1);   // Truncates, like Chromium.
}

TEST(TimeDeltaTest, FloatingPointConversions) {
  EXPECT_DOUBLE_EQ(Milliseconds(33).InSecondsF(), 0.033);
  EXPECT_DOUBLE_EQ(Microseconds(1500).InMillisecondsF(), 1.5);
}

TEST(TimeDeltaTest, Arithmetic) {
  EXPECT_EQ(Seconds(1) + Milliseconds(500), Milliseconds(1500));
  EXPECT_EQ(Seconds(1) - Milliseconds(500), Milliseconds(500));
  EXPECT_EQ(Seconds(1) * 3, Seconds(3));
  EXPECT_EQ(3 * Seconds(1), Seconds(3));
  EXPECT_EQ(Seconds(3) / 3, Seconds(1));
  EXPECT_EQ(-Seconds(1), Seconds(-1));
  EXPECT_EQ(Seconds(2) / Seconds(1), 2.0);
}

TEST(TimeDeltaTest, Comparisons) {
  EXPECT_LT(Milliseconds(1), Seconds(1));
  EXPECT_GT(Seconds(1), Milliseconds(1));
  EXPECT_EQ(Seconds(1), Milliseconds(1000));
  EXPECT_TRUE(Milliseconds(-1).is_negative());
  EXPECT_FALSE(Milliseconds(0).is_negative());
}

TEST(TimeDeltaTest, InfinitySentinels) {
  EXPECT_TRUE(TimeDelta::Max().is_infinte());
  EXPECT_TRUE(TimeDelta::Min().is_infinte());
  EXPECT_TRUE(TimeDelta::Max().is_max());
  EXPECT_TRUE(TimeDelta::Min().is_min());
  EXPECT_FALSE(Seconds(1).is_infinte());
}

// Saturating arithmetic matters here: media timestamps are int64 microseconds
// and a bogus container can produce values near INT64_MAX. Overflow would wrap
// into a negative duration and silently invert every scheduling comparison.
TEST(TimeDeltaTest, AdditionSaturates) {
  EXPECT_EQ(TimeDelta::Max() + Seconds(1), TimeDelta::Max());
  EXPECT_EQ(TimeDelta::Min() - Seconds(1), TimeDelta::Min());
  EXPECT_EQ(TimeDelta::Max() + TimeDelta::Max(), TimeDelta::Max());
}

TEST(TimeDeltaTest, MultiplicationSaturates) {
  EXPECT_EQ(TimeDelta::Max() * 2, TimeDelta::Max());
  EXPECT_EQ(Seconds(1) * INT64_MAX, TimeDelta::Max());
  EXPECT_EQ(Seconds(-1) * INT64_MAX, TimeDelta::Min());
}

TEST(TimeDeltaTest, DivisionByZeroDoesNotTrap) {
  EXPECT_EQ(Seconds(1) / 0, TimeDelta::Max());
  EXPECT_EQ(Seconds(-1) / 0, TimeDelta::Min());
}

TEST(TimeDeltaTest, Modulo) {
  EXPECT_EQ(Milliseconds(100) % Milliseconds(30), Milliseconds(10));
}

TEST(TimeDeltaTest, ToStringIsHumanReadable) {
  EXPECT_EQ(Seconds(2).ToString(), "2.000000s");
  EXPECT_EQ(Milliseconds(33).ToString(), "33.000000ms");
  EXPECT_EQ(TimeDelta::Max().ToString(), "inf");
}

TEST(TimeTicksTest, DefaultIsNull) {
  EXPECT_TRUE(TimeTicks().is_null());
  EXPECT_FALSE(TimeTicks::FromMicroseconds(1).is_null());
}

TEST(TimeTicksTest, DifferenceYieldsTimeDelta) {
  const TimeTicks a = TimeTicks::FromMicroseconds(1000);
  const TimeTicks b = TimeTicks::FromMicroseconds(2500);
  EXPECT_EQ(b - a, Microseconds(1500));
  EXPECT_EQ(a - b, Microseconds(-1500));
  EXPECT_EQ(a + Microseconds(500), TimeTicks::FromMicroseconds(1500));
}

TEST(TimeTicksTest, NowIsMonotonic) {
  const TimeTicks t0 = TimeTicks::Now();
  const TimeTicks t1 = TimeTicks::Now();
  EXPECT_GE(t1, t0);
  EXPECT_FALSE(t0.is_null());
}

TEST(SimpleTestTickClockTest, OnlyAdvancesWhenTold) {
  SimpleTestTickClock clock;
  const TimeTicks start = clock.NowTicks();
  EXPECT_EQ(clock.NowTicks(), start);
  EXPECT_EQ(clock.NowTicks(), start);

  clock.Advance(Milliseconds(250));
  EXPECT_EQ(clock.NowTicks() - start, Milliseconds(250));

  clock.StartAt(TimeTicks::FromMicroseconds(5000000));
  EXPECT_EQ(clock.NowTicks(), TimeTicks::FromMicroseconds(5000000));
}

TEST(SimpleTestTickClockTest, SatisfiesTickClockInterface) {
  SimpleTestTickClock clock;
  const TickClock* as_interface = &clock;
  const TimeTicks before = as_interface->NowTicks();
  clock.Advance(Seconds(1));
  EXPECT_EQ(as_interface->NowTicks() - before, Seconds(1));
}

TEST(TimeTest, UnixConversionRoundTrips) {
  const Time t = Time::FromUnixMillis(1700000000123LL);
  EXPECT_EQ(t.ToUnixMillis(), 1700000000123LL);
}

}  // namespace
}  // namespace ijkpp::base
