// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/synchronization/lock.h"

#include <memory>
#include <thread>
#include <vector>

#include "base/sequence_checker.h"
#include "base/synchronization/atomic_flag.h"
#include "base/synchronization/atomic_sequence_number.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/waitable_event.h"
#include "gtest/gtest.h"

namespace avbase::base {
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
  for (auto& t : threads)
    t.join();
  EXPECT_EQ(counter, 8 * 5000);
}

TEST(LockTest, TryReportsContention) {
  Lock lock;
  AutoLock scoped(lock);
  EXPECT_FALSE(
      lock.Try());  // Held by this thread; std::mutex is not recursive.
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

// The lock-order checker records, per thread, the orderings of the locks that
// thread acquired. Both of its containers have to be per-thread: recording an
// ordering from two threads at once is a data race on a std::vector. That is
// what TSan reports here, and it is not theoretical -- in a RelWithDebInfo
// build the same race corrupted the heap and aborted a playback run in about
// one attempt out of thirty under load (libmalloc: "pointer being freed was
// not allocated"), which is how the missing thread_local was found.
TEST(LockTest, ConcurrentOrderRecordingIsRaceFree) {
  // The checker only *writes* its ordering table until it has seen a given
  // pair, so a test that takes the same two locks over and over stops
  // exercising the write path immediately -- and then no race shows up. Each
  // thread gets its own set of locks and walks every (i, j) pair with i < j,
  // which keeps producing new pairs for a while.
  //
  // Two properties matter and both are load-bearing:
  //   * the locks are allocated once per thread, so their addresses are
  //     stable. Allocating per iteration recycles addresses, and a recycled
  //     pair can look like the *reverse* of one already recorded -- the
  //     inversion check then fires and aborts the test (which is how this
  //     version was found).
  //   * every thread acquires in ascending index order, so its own records are
  //     never reversed either.
  constexpr int kLocksPerThread = 24;
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([] {
      std::vector<std::unique_ptr<Lock>> locks;
      locks.reserve(static_cast<size_t>(kLocksPerThread));
      for (int i = 0; i < kLocksPerThread; ++i) {
        locks.push_back(std::make_unique<Lock>());
      }
      for (int i = 0; i < kLocksPerThread; ++i) {
        for (int j = i + 1; j < kLocksPerThread; ++j) {
          AutoLock first(*locks[static_cast<size_t>(i)]);
          AutoLock second(*locks[static_cast<size_t>(j)]);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
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
  EXPECT_TRUE(event.IsSignaled());  // Manual reset by default.
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
      for (int j = 0; j < 10000; ++j)
        seq.GetNext();
    });
  }
  for (auto& t : threads)
    t.join();
  EXPECT_EQ(seq.Get(), 8 * 10000);
}

TEST(SequenceCheckerTest, BindsOnFirstUse) {
  SequenceChecker checker;
  EXPECT_TRUE(checker.CalledOnValidSequence());
  EXPECT_TRUE(checker.CalledOnValidSequence());
}

TEST(SequenceCheckerTest, RejectsOtherThreads) {
  SequenceChecker checker;
  ASSERT_TRUE(checker.CalledOnValidSequence());  // Bind to this thread.

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
}  // namespace avbase::base
