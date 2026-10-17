// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/buffer_controller.h"

#include "gtest/gtest.h"

namespace avbase::player {
namespace {

constexpr base::TimeDelta kFirst = base::Milliseconds(100);
constexpr base::TimeDelta kNext = base::Milliseconds(500);
constexpr base::TimeDelta kLast = base::Milliseconds(4000);

BufferController::Thresholds TestThresholds() {
  return BufferController::Thresholds{kFirst, kNext, kLast};
}

TEST(BufferControllerTest, InitialStartWaitsForTheFirstMark) {
  BufferController c(TestThresholds());
  c.OnBufferingStart();

  EXPECT_EQ(c.Evaluate(base::Milliseconds(99)),
            BufferController::Decision::kKeepWaiting);
  EXPECT_EQ(c.Evaluate(kFirst), BufferController::Decision::kProceed);
  EXPECT_EQ(c.Evaluate(base::Milliseconds(5000)),
            BufferController::Decision::kProceed);
}

TEST(BufferControllerTest, RecoveryAdvancesFirstToNext) {
  BufferController c(TestThresholds());
  c.OnBufferingStart();
  ASSERT_EQ(c.Evaluate(kFirst), BufferController::Decision::kProceed);
  c.OnBufferingEnd();

  // The bar moved: the same 100 ms that started playback no longer recovers
  // it. A second stall must wait for the next tier.
  c.OnBufferingStart();
  EXPECT_EQ(c.Evaluate(kFirst), BufferController::Decision::kKeepWaiting);
  EXPECT_EQ(c.Evaluate(kNext), BufferController::Decision::kProceed);
  EXPECT_EQ(c.current_mark(), kNext);
}

TEST(BufferControllerTest, StepSaturatesAtLast) {
  BufferController c(TestThresholds());
  for (int i = 0; i < 5; ++i) {
    c.OnBufferingStart();
    c.OnBufferingEnd();
  }
  EXPECT_EQ(c.step(), BufferController::Step::kLast);
  EXPECT_EQ(c.current_mark(), kLast);
}

TEST(BufferControllerTest, SeekResetsTheProgression) {
  BufferController c(TestThresholds());
  c.OnBufferingStart();
  c.OnBufferingEnd();
  c.OnBufferingStart();
  c.OnBufferingEnd();
  ASSERT_EQ(c.step(), BufferController::Step::kLast);

  c.OnSeekCompleted();
  EXPECT_EQ(c.step(), BufferController::Step::kFirst);
  EXPECT_EQ(c.current_mark(), kFirst);
  // And the fresh position waits for the first mark again, not the last:
  // 600 ms was comfortably past the (stale) kLast tier's gate and must now
  // count fully toward the much smaller first mark.
  c.OnBufferingStart();
  EXPECT_EQ(c.Evaluate(base::Milliseconds(99)),
            BufferController::Decision::kKeepWaiting);
  EXPECT_EQ(c.Evaluate(kFirst), BufferController::Decision::kProceed);
}

TEST(BufferControllerTest, SeekClosesAnOpenCycle) {
  BufferController c(TestThresholds());
  c.OnBufferingStart();
  c.OnSeekCompleted();

  // The seek closed the cycle; the recovery that follows must NOT advance
  // the tier, because no buffering decision was actually honored.
  c.OnBufferingEnd();
  EXPECT_EQ(c.step(), BufferController::Step::kFirst);
  EXPECT_FALSE(c.buffering());
}

TEST(BufferControllerTest, NotBufferingAlwaysProceeds) {
  BufferController c(TestThresholds());
  EXPECT_EQ(c.Evaluate(base::TimeDelta()),
            BufferController::Decision::kProceed);
  c.OnBufferingStart();
  c.OnBufferingEnd();
  EXPECT_EQ(c.Evaluate(base::TimeDelta()),
            BufferController::Decision::kProceed);
}

TEST(BufferControllerTest, RedundantStartDoesNotResetTheCycle) {
  BufferController c(TestThresholds());
  c.OnBufferingStart();
  c.OnBufferingStart();  // The renderer may re-report starvation.
  c.OnBufferingEnd();
  EXPECT_EQ(c.step(), BufferController::Step::kNext)
      << "a duplicated start must not swallow the advance";
}

TEST(BufferControllerTest, SpuriousEndWithoutStartDoesNotAdvance) {
  BufferController c(TestThresholds());
  c.OnBufferingEnd();
  EXPECT_EQ(c.step(), BufferController::Step::kFirst);
}

TEST(BufferControllerTest, ProgressIsClamped) {
  BufferController c(TestThresholds());
  c.OnBufferingStart();
  EXPECT_EQ(c.progress_percent(base::TimeDelta()), 0);
  EXPECT_EQ(c.progress_percent(base::Milliseconds(50)), 50);
  EXPECT_EQ(c.progress_percent(base::Milliseconds(99)), 99);
  EXPECT_EQ(c.progress_percent(base::Milliseconds(100)), 100);
  EXPECT_EQ(c.progress_percent(base::Milliseconds(99999)), 100);
}

TEST(BufferControllerTest, ThresholdsAreForcedToAscend) {
  // A config with next < first would make recovery easier than starting;
  // the controller repairs the order instead of honoring it.
  BufferController c(BufferController::Thresholds{
      base::Milliseconds(500), base::Milliseconds(100), kLast});
  c.OnBufferingStart();
  c.OnBufferingEnd();
  c.OnBufferingStart();
  EXPECT_EQ(c.current_mark(), base::Milliseconds(500));
}

TEST(BufferControllerTest, ConfigCtorDerivesMarksFromBufferConfig) {
  BufferConfig config;
  config.first_high_water_mark = base::Milliseconds(200);
  config.next_high_water_mark = base::Milliseconds(800);
  config.last_high_water_mark = base::Milliseconds(3000);
  config.enabled = true;

  BufferController c(config);
  EXPECT_TRUE(c.enabled());
  EXPECT_EQ(c.current_mark(), base::Milliseconds(200));
  c.OnBufferingStart();
  EXPECT_EQ(c.Evaluate(base::Milliseconds(199)),
            BufferController::Decision::kKeepWaiting);
  EXPECT_EQ(c.Evaluate(base::Milliseconds(200)),
            BufferController::Decision::kProceed);
}

TEST(BufferControllerTest, ConfigCtorRepairsANonAscendingMarkSet) {
  // A BufferConfig whose next-mark sits below the first is repaired the same
  // way the Thresholds ctor repairs it, because the fix lives in Normalize.
  BufferConfig config;
  config.first_high_water_mark = base::Milliseconds(500);
  config.next_high_water_mark = base::Milliseconds(100);
  config.last_high_water_mark = base::Milliseconds(4000);

  BufferController c(config);
  EXPECT_EQ(c.current_mark(), base::Milliseconds(500));
  c.OnBufferingStart();
  c.OnBufferingEnd();  // advance first -> next
  c.OnBufferingStart();
  EXPECT_EQ(c.current_mark(), base::Milliseconds(500));  // next raised to first
}

TEST(BufferControllerTest, DisabledGateAlwaysProceedsAndReportsComplete) {
  // packet-buffering=false: the time gate is off, so even an empty buffer
  // proceeds (real underflow still pauses through the renderer's own check).
  BufferConfig config;
  config.enabled = false;

  BufferController c(config);
  EXPECT_FALSE(c.enabled());
  c.OnBufferingStart();
  EXPECT_EQ(c.Evaluate(base::TimeDelta()),
            BufferController::Decision::kProceed);
  EXPECT_EQ(c.progress_percent(base::TimeDelta()), 100);
}

}  // namespace
}  // namespace avbase::player
