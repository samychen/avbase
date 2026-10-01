// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/threading/thread.h"

#include <atomic>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/waitable_event.h"
#include "gtest/gtest.h"

namespace avbase::base {
namespace {

TEST(ThreadTest, StartsAndRunsPostedTasks) {
  Thread thread("avbase-test");
  ASSERT_TRUE(thread.Start());
  EXPECT_TRUE(thread.IsRunning());
  EXPECT_EQ(thread.name(), "avbase-test");

  WaitableEvent done;
  std::atomic<int> value{0};
  ASSERT_TRUE(thread.task_runner()->PostTask(
      FROM_HERE, BindOnce([&value, &done]() {
        value.store(42);
        done.Signal();
      })));
  ASSERT_TRUE(done.TimedWait(Seconds(5)));
  EXPECT_EQ(value.load(), 42);
}

TEST(ThreadTest, TaskRunnerIsUsableBeforeStart) {
  Thread thread("avbase-early");
  // Tasks posted before Start() are queued and run once the loop begins, so a
  // caller never has to synchronise on thread startup.
  std::atomic<int> value{0};
  ASSERT_TRUE(thread.task_runner()->PostTask(
      FROM_HERE, BindOnce([&value]() { value.store(7); })));
  ASSERT_TRUE(thread.Start());
  for (int i = 0; i < 500 && value.load() == 0; ++i) {
    std::this_thread::sleep_for(Milliseconds(1).ToChronoMicros());
  }
  EXPECT_EQ(value.load(), 7);
}

TEST(ThreadTest, SetsTheOsThreadName) {
  Thread thread("avbase-named");
  ASSERT_TRUE(thread.Start());

  WaitableEvent done;
  std::string observed;
  ASSERT_TRUE(thread.task_runner()->PostTask(
      FROM_HERE, BindOnce([&observed, &done]() {
        observed = PlatformThread::GetName();
        done.Signal();
      })));
  ASSERT_TRUE(done.TimedWait(Seconds(5)));
  EXPECT_EQ(observed, "avbase-named");
}

// Linux truncates thread names to 15 characters; the wrapper must not pass an
// over-long buffer to pthread_setname_np, which would fail with ERANGE.
TEST(ThreadTest, LongNameIsTruncatedNotRejected) {
  Thread thread("avbase-this-name-is-far-too-long");
  ASSERT_TRUE(thread.Start());

  WaitableEvent done;
  std::string observed;
  ASSERT_TRUE(thread.task_runner()->PostTask(
      FROM_HERE, BindOnce([&observed, &done]() {
        observed = PlatformThread::GetName();
        done.Signal();
      })));
  ASSERT_TRUE(done.TimedWait(Seconds(5)));
  EXPECT_LE(observed.size(), 15u);
  EXPECT_EQ(observed, "avbase-this-nam");
}

// The guarantee behind Δ15 and docs/04 §5.4: Stop() returns even when the
// sequence is idle and blocked in a condition wait, and no task runs after it.
TEST(ThreadTest, StopIsBoundedAndIdempotent) {
  Thread thread("avbase-stop");
  ASSERT_TRUE(thread.Start());

  std::atomic<int> after_stop{0};
  const TimeTicks before = TimeTicks::Now();
  thread.Stop();
  const TimeDelta elapsed = TimeTicks::Now() - before;

  EXPECT_LT(elapsed, Seconds(2)) << "Stop() blocked for " << elapsed.ToString();
  EXPECT_FALSE(thread.IsRunning());
  // PostTask after Stop() must fail rather than silently queue.
  EXPECT_FALSE(thread.task_runner()->PostTask(
      FROM_HERE, BindOnce([&after_stop]() { after_stop.fetch_add(1); })));
  EXPECT_EQ(after_stop.load(), 0);

  thread.Stop();   // Idempotent.
  SUCCEED();
}

TEST(ThreadTest, DestructorStopsTheThread) {
  std::atomic<bool> was_running{false};
  {
    Thread thread("avbase-dtor");
    ASSERT_TRUE(thread.Start());
    was_running.store(thread.IsRunning());
  }
  EXPECT_TRUE(was_running.load());
  // If ~Thread() had not joined, this test binary would crash or hang here.
  SUCCEED();
}

// The structural guarantee that removes ijkplayer's "callback fired after the
// player was released" crash class: a WeakPtr bound into a posted task goes
// inert when its owner dies, even though the task is already queued.
TEST(ThreadTest, WeakPtrBoundTaskIsInertAfterOwnerDies) {
  // Counter lives outside Target so the recording survives the target's death
  // and the test can assert on it afterwards.
  auto touches = std::make_shared<std::atomic<int>>(0);

  class Target {
   public:
    explicit Target(std::shared_ptr<std::atomic<int>> counter)
        : counter_(std::move(counter)) {}
    void Touch() { counter_->fetch_add(1); }
    WeakPtr<Target> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

   private:
    std::shared_ptr<std::atomic<int>> counter_;
    // Last member on purpose: it must invalidate every WeakPtr before the rest
    // of the object goes away (check_invariants rule C19).
    WeakPtrFactory<Target> weak_factory_{this};
  };

  Thread thread("avbase-weak");
  ASSERT_TRUE(thread.Start());

  {
    Target target(touches);
    // While the target is alive the bound call runs.
    WaitableEvent ran;
    // Bind a MEMBER FUNCTION with the WeakPtr as receiver — the canonical
    // Chromium form. Binding a lambda that takes a raw T* also works at runtime
    // (BindState's AnyWeakPtrInvalid guard skips the call), but GCC correctly
    // warns at -O2 that WeakPtr::get() may be null and the lambda parameter is
    // a bare pointer: the compiler cannot see the guard. Binding the member
    // function keeps the null check inside the generated call, so the warning
    // disappears and the intent is clearer.
    //
    // Also note: bind the WeakPtr itself, never WeakPtr::get(). The raw pointer
    // compiles, looks equivalent, and silently disables the liveness guard —
    // verified the hard way when this test touched a destroyed object.
    ASSERT_TRUE(thread.task_runner()->PostTask(
        FROM_HERE,
        BindOnce([](Target* t, WaitableEvent* ev) {
                   t->Touch();
                   ev->Signal();
                 },
                 target.GetWeakPtr(), &ran)));
    ASSERT_TRUE(ran.TimedWait(Seconds(5)));
    EXPECT_EQ(touches->load(), 1);

    // Now queue a long-delayed call bound to the WeakPtr, then destroy the
    // target before it can run.
    // The delayed call is bound to a method that would bump the counter to a
    // recognisable value; if the WeakPtr guard failed we would see it.
    thread.task_runner()->PostDelayedTask(
        FROM_HERE, BindOnce(&Target::Touch, target.GetWeakPtr()),
        Milliseconds(200));
  }   // |target| is gone; its WeakPtrFactory has invalidated the pointer.

  // Drain past the delayed task's due time. If the WeakPtr guard failed, this
  // would dereference freed memory (ASan) or set touches to -1.
  std::this_thread::sleep_for(Milliseconds(400).ToChronoMicros());
  // Still 1: the delayed Touch() was skipped because the WeakPtr went invalid.
  EXPECT_EQ(touches->load(), 1);
  thread.Stop();
}

TEST(ThreadTest, StartTwiceReturnsFalse) {
  Thread thread("avbase-twice");
  ASSERT_TRUE(thread.Start());
  EXPECT_FALSE(thread.Start());
  thread.Stop();
}

TEST(ThreadTest, MultipleThreadsAreIndependent) {
  Thread a("avbase-a"), b("avbase-b");
  ASSERT_TRUE(a.Start());
  ASSERT_TRUE(b.Start());

  std::atomic<int> counter_a{0}, counter_b{0};
  WaitableEvent done_a, done_b;
  a.task_runner()->PostTask(FROM_HERE, BindOnce([&counter_a, &done_a]() {
    for (int i = 0; i < 1000; ++i) counter_a.fetch_add(1);
    done_a.Signal();
  }));
  b.task_runner()->PostTask(FROM_HERE, BindOnce([&counter_b, &done_b]() {
    for (int i = 0; i < 1000; ++i) counter_b.fetch_add(1);
    done_b.Signal();
  }));
  ASSERT_TRUE(done_a.TimedWait(Seconds(5)));
  ASSERT_TRUE(done_b.TimedWait(Seconds(5)));
  EXPECT_EQ(counter_a.load(), 1000);
  EXPECT_EQ(counter_b.load(), 1000);
  EXPECT_NE(a.GetThreadId(), b.GetThreadId());
}

}  // namespace
}  // namespace avbase::base
