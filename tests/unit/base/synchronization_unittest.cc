// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/synchronization/lock.h"

#include <thread>
#include <vector>

#include "base/synchronization/atomic_flag.h"
#include "base/synchronization/atomic_sequence_number.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/waitable_event.h"
#include "base/sequence_checker.h"
#include "gtest/gtest.h"

namespace ijkpp::base {
namespace {

TEST(LockTest, AutoLockGuardsCriticalSection) {
  Lock lock;
  int counter = 0;
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&lock, &counter]() {
      for (int j = 0; j < 5000; ++j) {
        AutoLock scoped(lock);
        ++counter;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(counter, 8 * 5000);
}

TEST(LockTest, TryReportsContention) {
  Lock lock;
  AutoLock scoped(lock);
  EXPECT_FALSE(lock.Try());   // Held by this thread; std::mutex is not recursive.
}

TEST(LockTest, AutoUnlockReacquiresOnScopeExit) {
  Lock lock;
  AutoLock scoped(lock);
  {
    AutoUnlock release(lock);
    EXPECT_TRUE(lock.Try());
    lock.Release();
  }
  // |scoped| still owns the lock here, so a plain Try() must fail.
  EXPECT_FALSE(lock.Try());
}

TEST(ConditionVariableTest, SignalWakesOneWaiter) {
  Lock lock;
  ConditionVariable cv(&lock);
  bool ready = false;
  bool woke = false;

  std::thread waiter([&]() {
    AutoLock scoped(lock);
    while (!ready) {
      cv.Wait();
    }
    woke = true;
  });

  {
    AutoLock scoped(lock);
    ready = true;
    cv.Signal();
  }
  waiter.join();
  EXPECT_TRUE(woke);
}

TEST(ConditionVariableTest, TimedWaitReturnsFalseOnTimeout) {
  Lock lock;
  ConditionVariable cv(&lock);
  AutoLock scoped(lock);
  EXPECT_FALSE(cv.TimedWait(Milliseconds(1)));
}

TEST(WaitableEventTest, ManualResetStaysSignaled) {
  WaitableEvent event;
  EXPECT_FALSE(event.IsSignaled());
  event.Signal();
  EXPECT_TRUE(event.IsSignaled());
  event.Wait();
  EXPECT_TRUE(event.IsSignaled());   // Manual reset by default.
  EXPECT_TRUE(event.TimedWait(Milliseconds(1)));
}

TEST(WaitableEventTest, AutomaticResetClearsAfterWait) {
  WaitableEvent event(WaitableEvent::ResetPolicy::kAutomaticReset,
                      WaitableEvent::InitialState::kNotSignaled);
  event.Signal();
  event.Wait();
  EXPECT_FALSE(event.IsSignaled());
  EXPECT_FALSE(event.TimedWait(Milliseconds(1)));
}

TEST(WaitableEventTest, TimedWaitTimesOut) {
  WaitableEvent event;
  const TimeTicks before = TimeTicks::Now();
  EXPECT_FALSE(event.TimedWait(Milliseconds(20)));
  EXPECT_GE(TimeTicks::Now() - before, Milliseconds(15));
}

// This is the mechanism that keeps ~Player() bounded (docs/04 §5.4, Δ15).
TEST(WaitableEventTest, SignalFromAnotherThreadUnblocksWaiter) {
  WaitableEvent event;
  std::thread signaler([&event]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    event.Signal();
  });
  EXPECT_TRUE(event.TimedWait(Seconds(5)));
  signaler.join();
}

TEST(AtomicFlagTest, OneWayLatch) {
  AtomicFlag flag;
  EXPECT_FALSE(flag.IsSet());
  flag.Set();
  EXPECT_TRUE(flag.IsSet());
  flag.Reset();
  EXPECT_FALSE(flag.IsSet());
}

TEST(AtomicFlagTest, VisibleAcrossThreads) {
  AtomicFlag flag;
  std::thread reader([&flag]() {
    while (!flag.IsSet()) {
      std::this_thread::yield();
    }
  });
  flag.Set();
  reader.join();
  SUCCEED();
}

TEST(AtomicSequenceNumberTest, AddReturnsNewValue) {
  AtomicSequenceNumber seq;
  EXPECT_EQ(seq.Get(), 0);
  EXPECT_EQ(seq.GetNext(), 1);
  EXPECT_EQ(seq.GetNext(), 2);
  EXPECT_EQ(seq.Add(5), 7);
  EXPECT_EQ(seq.Get(), 7);
}

TEST(AtomicSequenceNumberTest, ConcurrentIncrementsAreLossless) {
  AtomicSequenceNumber seq;
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&seq]() {
      for (int j = 0; j < 10000; ++j) seq.GetNext();
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(seq.Get(), 8 * 10000);
}

TEST(SequenceCheckerTest, BindsOnFirstUse) {
  SequenceChecker checker;
  EXPECT_TRUE(checker.CalledOnValidSequence());
  EXPECT_TRUE(checker.CalledOnValidSequence());
}

TEST(SequenceCheckerTest, RejectsOtherThreads) {
  SequenceChecker checker;
  ASSERT_TRUE(checker.CalledOnValidSequence());   // Bind to this thread.

  bool other_thread_ok = true;
  std::thread t([&checker, &other_thread_ok]() {
    other_thread_ok = checker.CalledOnValidSequence();
  });
  t.join();
  EXPECT_FALSE(other_thread_ok);
}

TEST(SequenceCheckerTest, DetachAllowsRebinding) {
  SequenceChecker checker;
  ASSERT_TRUE(checker.CalledOnValidSequence());

  bool other_thread_ok = false;
  {
    std::thread t([&checker, &other_thread_ok]() {
      other_thread_ok = checker.CalledOnValidSequence();
    });
    t.join();
  }
  ASSERT_FALSE(other_thread_ok);

  checker.DetachFromSequence();
  std::thread t2([&checker, &other_thread_ok]() {
    other_thread_ok = checker.CalledOnValidSequence();
  });
  t2.join();
  EXPECT_TRUE(other_thread_ok);
}

}  // namespace
}  // namespace ijkpp::base
