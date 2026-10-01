// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task/task_queue.h"

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/task/task_runner_util.h"
#include "base/test/task_environment.h"
#include "gtest/gtest.h"

namespace avbase::base {
namespace {

TEST(TaskQueueTest, RunsPostedTasksInOrder) {
  auto queue = MakeRefCounted<TaskQueue>();
  std::vector<int> order;
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(queue->PostTask(FROM_HERE, BindOnce([&order, i]() {
                                  order.push_back(i);
                                })));
  }
  EXPECT_EQ(queue->GetPendingTaskCount(), 5u);
  EXPECT_EQ(queue->RunAllReadyTasks(TimeTicks::Now()), 5u);
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
  EXPECT_EQ(queue->GetPendingTaskCount(), 0u);
}

TEST(TaskQueueTest, NullClosureIsRejected) {
  auto queue = MakeRefCounted<TaskQueue>();
  EXPECT_FALSE(queue->PostTask(FROM_HERE, OnceClosure()));
  EXPECT_EQ(queue->GetPendingTaskCount(), 0u);
}

TEST(TaskQueueTest, DelayedTasksRespectReadyTime) {
  auto queue = MakeRefCounted<TaskQueue>();
  SimpleTestTickClock clock;
  clock.StartAt(TimeTicks::FromMicroseconds(1000000));
  queue->SetTickClockForTesting(&clock);

  std::vector<int> order;
  queue->PostDelayedTask(FROM_HERE, BindOnce([&order]() { order.push_back(2); }),
                         Milliseconds(200));
  queue->PostDelayedTask(FROM_HERE, BindOnce([&order]() { order.push_back(1); }),
                         Milliseconds(100));
  queue->PostTask(FROM_HERE, BindOnce([&order]() { order.push_back(0); }));

  // Nothing beyond the immediate task is due yet.
  EXPECT_EQ(queue->RunAllReadyTasks(clock.NowTicks()), 1u);
  EXPECT_EQ(order, (std::vector<int>{0}));

  clock.Advance(Milliseconds(150));
  EXPECT_EQ(queue->RunAllReadyTasks(clock.NowTicks()), 1u);
  EXPECT_EQ(order, (std::vector<int>{0, 1}));

  clock.Advance(Milliseconds(150));
  EXPECT_EQ(queue->RunAllReadyTasks(clock.NowTicks()), 1u);
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2}));
}

// Ordering by (ready_time, sequence_number): a zero-delay task posted later
// must not overtake a delayed task that is already due. Without this invariant
// FastForwardBy() would be non-deterministic.
TEST(TaskQueueTest, DueDelayedTaskBeatsLaterImmediateTask) {
  auto queue = MakeRefCounted<TaskQueue>();
  SimpleTestTickClock clock;
  clock.StartAt(TimeTicks::FromMicroseconds(1000000));
  queue->SetTickClockForTesting(&clock);

  std::vector<int> order;
  queue->PostDelayedTask(FROM_HERE, BindOnce([&order]() { order.push_back(1); }),
                         Milliseconds(10));
  clock.Advance(Milliseconds(20));   // The delayed task is now due.
  queue->PostTask(FROM_HERE, BindOnce([&order]() { order.push_back(2); }));

  queue->RunAllReadyTasks(clock.NowTicks());
  EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(TaskQueueTest, GetNextReadyTimeReportsEarliest) {
  auto queue = MakeRefCounted<TaskQueue>();
  SimpleTestTickClock clock;
  clock.StartAt(TimeTicks::FromMicroseconds(1000000));
  queue->SetTickClockForTesting(&clock);

  EXPECT_TRUE(queue->GetNextReadyTime().is_null());
  queue->PostDelayedTask(FROM_HERE, DoNothing(), Milliseconds(300));
  queue->PostDelayedTask(FROM_HERE, DoNothing(), Milliseconds(100));
  EXPECT_EQ(queue->GetNextReadyTime(), clock.NowTicks() + Milliseconds(100));
}

TEST(TaskQueueTest, NegativeDelayIsTreatedAsImmediate) {
  auto queue = MakeRefCounted<TaskQueue>();
  SimpleTestTickClock clock;
  clock.StartAt(TimeTicks::FromMicroseconds(1000000));
  queue->SetTickClockForTesting(&clock);

  bool ran = false;
  queue->PostDelayedTask(FROM_HERE, BindOnce([&ran]() { ran = true; }),
                         Milliseconds(-500));
  EXPECT_EQ(queue->RunAllReadyTasks(clock.NowTicks()), 1u);
  EXPECT_TRUE(ran);
}

TEST(TaskQueueTest, ClearDropsPendingTasks) {
  auto queue = MakeRefCounted<TaskQueue>();
  int runs = 0;
  queue->PostTask(FROM_HERE, BindOnce([&runs]() { ++runs; }));
  queue->PostDelayedTask(FROM_HERE, BindOnce([&runs]() { ++runs; }), Seconds(1));
  EXPECT_EQ(queue->GetPendingTaskCount(), 2u);
  queue->Clear();
  EXPECT_EQ(queue->GetPendingTaskCount(), 0u);
  EXPECT_EQ(queue->RunAllReadyTasks(TimeTicks::Now()), 0u);
  EXPECT_EQ(runs, 0);
}

// A task that reposts itself must not hang RunAllReadyTasks.
TEST(TaskQueueTest, RunAllReadyTasksIsBounded) {
  auto queue = MakeRefCounted<TaskQueue>();
  // A fixed virtual clock is required here: RunAllReadyTasks() takes |now| as a
  // parameter and only runs tasks that are ready at that instant. Against the
  // real clock, tasks reposted *during* the drain get a later ready_time and
  // would be skipped, making the bound untestable.
  SimpleTestTickClock clock;
  clock.StartAt(TimeTicks::FromMicroseconds(1000000));
  queue->SetTickClockForTesting(&clock);
  const TimeTicks now = clock.NowTicks();
  int count = 0;
  // A self-reposting closure: each run enqueues the next one, so a naive
  // drain loop would never terminate.
  struct Reposter {
    scoped_refptr<TaskQueue> queue;
    int* count;
    void operator()() {
      if (++(*count) < 1000) {
        queue->PostTask(FROM_HERE, BindOnce(&Reposter::operator(), *this));
      }
    }
  };
  Reposter reposter{queue, &count};
  queue->PostTask(FROM_HERE, BindOnce(reposter));
  // The bound, not the task's own stop condition, terminates the loop.
  const size_t ran = queue->RunAllReadyTasks(now, 100);
  EXPECT_EQ(ran, 100u);
  EXPECT_EQ(count, 100);

  // A queued Reposter holds a scoped_refptr back to |queue|, which is a
  // reference cycle: without Clear() the queue would never be destroyed and
  // LeakSanitizer would (correctly) report it. See the RULE in task_queue.h.
  EXPECT_GT(queue->GetPendingTaskCount(), 0u);
  queue->Clear();
  EXPECT_EQ(queue->GetPendingTaskCount(), 0u);
}

TEST(TaskQueueTest, QuitRejectsFurtherPosts) {
  auto queue = MakeRefCounted<TaskQueue>();
  queue->Quit();
  EXPECT_TRUE(queue->IsQuitting());
  EXPECT_FALSE(queue->PostTask(FROM_HERE, DoNothing()));
  EXPECT_EQ(queue->GetPendingTaskCount(), 0u);
}

// Runs on a real background thread: the production path used by base::Thread.
TEST(TaskQueueTest, RunLoopExecutesOnItsOwnThread) {
  auto queue = MakeRefCounted<TaskQueue>();
  std::atomic<int> executed{0};
  std::thread runner([&queue]() { queue->Run(); });

  for (int i = 0; i < 100; ++i) {
    while (!queue->PostTask(FROM_HERE,
                            BindOnce([&executed]() { executed.fetch_add(1); }))) {
      std::this_thread::yield();
    }
  }
  // Give the loop time to drain, then shut down.
  for (int i = 0; i < 1000 && executed.load() < 100; ++i) {
    std::this_thread::sleep_for(Milliseconds(1).ToChronoMicros());
  }
  queue->Quit();
  runner.join();
  EXPECT_EQ(executed.load(), 100);
}

TEST(TaskQueueTest, RunsTasksInCurrentSequenceReflectsTheRunnerThread) {
  auto queue = MakeRefCounted<TaskQueue>();
  EXPECT_FALSE(queue->RunsTasksInCurrentSequence());

  std::atomic<bool> inside{false};
  std::thread runner([&queue]() { queue->Run(); });
  while (!queue->PostTask(FROM_HERE,
                          BindOnce([&queue, &inside]() {
                            inside = queue->RunsTasksInCurrentSequence();
                            queue->Quit();
                          }))) {
    std::this_thread::yield();
  }
  runner.join();
  EXPECT_TRUE(inside.load());
  EXPECT_FALSE(queue->RunsTasksInCurrentSequence());
}

TEST(TaskRunnerUtilTest, PostTaskAndReplyWithResultDeliversTheValue) {
  auto queue = MakeRefCounted<TaskQueue>();
  int received = 0;
  // Extra parentheses: the comma in the template argument list would otherwise
  // be parsed by the gtest macro as an argument separator.
  ASSERT_TRUE((PostTaskAndReplyWithResult<int, int>(
      queue.get(), FROM_HERE, []() { return 42; },
      BindOnce([](int* out, int v) { *out = v; }, &received))));
  queue->RunAllReadyTasks(TimeTicks::Now());
  EXPECT_EQ(received, 42);
}

}  // namespace
}  // namespace avbase::base
