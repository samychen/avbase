// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/test/task_environment.h"

#include <functional>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "gtest/gtest.h"

namespace avbase::base {
namespace {

class TaskEnvironmentTest : public ::testing::Test {
 protected:
  test::TaskEnvironment task_environment_;
};

TEST_F(TaskEnvironmentTest, BecomesTheCurrentDefaultRunner) {
  ASSERT_TRUE(SequencedTaskRunner::HasCurrentDefault());
  EXPECT_EQ(SequencedTaskRunner::GetCurrentDefault().get(),
            task_environment_.GetMainThreadTaskRunner());
  EXPECT_TRUE(task_environment_.GetMainThreadTaskRunner()
                  ->RunsTasksInCurrentSequence());
}

TEST_F(TaskEnvironmentTest, RunUntilIdleDrainsPostedTasks) {
  std::vector<int> order;
  auto* runner = task_environment_.GetMainThreadTaskRunner();
  for (int i = 0; i < 5; ++i) {
    runner->PostTask(FROM_HERE,
                     BindOnce([&order, i]() { order.push_back(i); }));
  }
  EXPECT_EQ(task_environment_.GetPendingTaskCount(), 5u);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
  EXPECT_EQ(task_environment_.GetPendingTaskCount(), 0u);
}

TEST_F(TaskEnvironmentTest, RunUntilIdleFollowsChainedTasks) {
  std::vector<int> order;
  auto* runner = task_environment_.GetMainThreadTaskRunner();
  // Each task posts the next, so a single drain pass is not enough.
  std::function<void(int)> chain = [&runner, &order, &chain](int i) {
    order.push_back(i);
    if (i < 4) {
      runner->PostTask(FROM_HERE, BindOnce(chain, i + 1));
    }
  };
  runner->PostTask(FROM_HERE, BindOnce(chain, 0));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST_F(TaskEnvironmentTest, MockTimeDoesNotAdvanceOnItsOwn) {
  const TimeTicks before = task_environment_.NowTicks();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(task_environment_.NowTicks(), before);
}

TEST_F(TaskEnvironmentTest, RunUntilIdleSkipsFutureDelayedTasks) {
  bool ran = false;
  task_environment_.GetMainThreadTaskRunner()->PostDelayedTask(
      FROM_HERE, BindOnce([&ran]() { ran = true; }), Seconds(10));
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(ran);
  EXPECT_TRUE(task_environment_.HasDelayedTasks());
}

TEST_F(TaskEnvironmentTest, FastForwardByRunsDueTasksInTimeOrder) {
  std::vector<int> order;
  auto* runner = task_environment_.GetMainThreadTaskRunner();
  runner->PostDelayedTask(FROM_HERE, BindOnce([&order]() { order.push_back(3); }),
                          Milliseconds(300));
  runner->PostDelayedTask(FROM_HERE, BindOnce([&order]() { order.push_back(1); }),
                          Milliseconds(100));
  runner->PostDelayedTask(FROM_HERE, BindOnce([&order]() { order.push_back(2); }),
                          Milliseconds(200));

  const TimeTicks start = task_environment_.NowTicks();
  task_environment_.FastForwardBy(Milliseconds(250));
  EXPECT_EQ(order, (std::vector<int>{1, 2}));
  EXPECT_EQ(task_environment_.NowTicks() - start, Milliseconds(250));
  EXPECT_TRUE(task_environment_.HasDelayedTasks());

  task_environment_.FastForwardBy(Milliseconds(100));
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  EXPECT_FALSE(task_environment_.HasDelayedTasks());
}

// This is the property that replaces sleeping in tests: a 10-second timeout
// path is exercised in microseconds.
TEST_F(TaskEnvironmentTest, FastForwardByExercisesLongTimeoutsInstantly) {
  bool timed_out = false;
  task_environment_.GetMainThreadTaskRunner()->PostDelayedTask(
      FROM_HERE, BindOnce([&timed_out]() { timed_out = true; }), Seconds(30));

  const TimeTicks start = TimeTicks::Now();
  task_environment_.FastForwardBy(Seconds(30));
  EXPECT_TRUE(timed_out);
  EXPECT_LT(TimeTicks::Now() - start, Seconds(1))
      << "FastForwardBy must not actually wait";
}

// Under MOCK_TIME the code under test must read the *injected* clock. Anything
// calling TimeTicks::Now() directly is invisible to FastForwardBy() — which is
// exactly why design principle P6 forbids it in media/ and player/.
TEST_F(TaskEnvironmentTest, FastForwardBySeesIntermediateClockValues) {
  const TickClock* clock = task_environment_.GetTickClock();
  const TimeTicks start = clock->NowTicks();
  std::vector<int64_t> observed_ms;
  auto* runner = task_environment_.GetMainThreadTaskRunner();

  // First task at +100ms; it posts a second at +100ms from its own view of now.
  // Jumping straight to +250ms would run both back-to-back and hide the fact
  // that the second one should have seen t=200ms.
  runner->PostDelayedTask(
      FROM_HERE,
      BindOnce(
          [](const TickClock* c, std::vector<int64_t>* out,
             SequencedTaskRunner* r, int64_t base_micros) {
            out->push_back(
                (c->NowTicks() - TimeTicks::FromMicroseconds(base_micros))
                    .InMilliseconds());
            r->PostDelayedTask(
                FROM_HERE,
                BindOnce(
                    [](const TickClock* c2, std::vector<int64_t>* o,
                       int64_t b) {
                      o->push_back(
                          (c2->NowTicks() - TimeTicks::FromMicroseconds(b))
                              .InMilliseconds());
                    },
                    c, out, base_micros),
                Milliseconds(100));
          },
          clock, &observed_ms, runner, start.since_origin_micros()),
      Milliseconds(100));

  task_environment_.FastForwardBy(Milliseconds(250));
  ASSERT_EQ(observed_ms.size(), 2u);
  EXPECT_EQ(observed_ms[0], 100);
  EXPECT_EQ(observed_ms[1], 200);
}

TEST_F(TaskEnvironmentTest, AdvanceClockDoesNotRunTasks) {
  bool ran = false;
  task_environment_.GetMainThreadTaskRunner()->PostDelayedTask(
      FROM_HERE, BindOnce([&ran]() { ran = true; }), Milliseconds(10));
  task_environment_.AdvanceClock(Milliseconds(50));
  EXPECT_FALSE(ran);   // Clock moved, queue untouched.
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(ran);
}

TEST_F(TaskEnvironmentTest, RealTimeSourceUsesTheSystemClock) {
  test::TaskEnvironment real_time(test::TaskEnvironment::TimeSource::kRealTime);
  const TimeTicks before = real_time.NowTicks();
  std::this_thread::sleep_for(Milliseconds(5).ToChronoMicros());
  EXPECT_GT(real_time.NowTicks(), before);

  bool ran = false;
  real_time.GetMainThreadTaskRunner()->PostTask(
      FROM_HERE, BindOnce([&ran]() { ran = true; }));
  real_time.RunUntilIdle();
  EXPECT_TRUE(ran);
}

TEST_F(TaskEnvironmentTest, NestedEnvironmentsRestoreThePreviousDefault) {
  auto* outer = task_environment_.GetMainThreadTaskRunner();
  {
    test::TaskEnvironment inner;
    EXPECT_NE(inner.GetMainThreadTaskRunner(), outer);
    EXPECT_EQ(SequencedTaskRunner::GetCurrentDefault().get(),
              inner.GetMainThreadTaskRunner());
  }
  EXPECT_EQ(SequencedTaskRunner::GetCurrentDefault().get(), outer);
}

}  // namespace
}  // namespace avbase::base
