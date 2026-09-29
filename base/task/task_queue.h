// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_BASE_TASK_TASK_QUEUE_H_
#define IJKPP_BASE_TASK_TASK_QUEUE_H_

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "base/base_export.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"

namespace ijkpp::base {

// A SequencedTaskRunner backed by one ordered queue plus a run loop.
//
// This is ijkpp's message loop. Chromium splits the same responsibility across
// base::MessageLoop, base::MessagePump and base::TaskQueue; ijkpp collapses
// them because the core needs neither fd watching nor nested RunLoops — the
// platform layer runs its own poll loop for Wayland/epoll (docs/04 §2.1, D3).
// If fd watching is ever needed in base/, split MessagePump back out behind the
// same interface; callers will not change.
//
// Ordering is by (ready_time, insertion_sequence), so a task posted with zero
// delay never overtakes an already-due delayed task. That is what makes
// TaskEnvironment::FastForwardBy() deterministic.
//
// RULE: a task must never hold a scoped_refptr to the TaskQueue it is posted
// to. The queue owns the task, so that is a reference cycle and the queue will
// never be destroyed — LeakSanitizer reports it and it is a real leak, not a
// false positive. Capture a base::WeakPtr instead, or call Clear() before
// dropping your own reference.
class IJKPP_BASE_EXPORT TaskQueue final : public SequencedTaskRunner {
 public:
  TaskQueue();
  TaskQueue(const TaskQueue&) = delete;
  TaskQueue& operator=(const TaskQueue&) = delete;
  ~TaskQueue() override;

  // Runs tasks on the calling thread until Quit() is called from another thread
  // or from a task. Blocks. Only one thread may call this at a time.
  void Run();

  // Thread-safe. Wakes Run() and makes it return. Tasks posted after this are
  // dropped and PostTask returns false, which is how a caller learns that the
  // target sequence is gone.
  void Quit();
  bool IsQuitting() const { return quit_.load(std::memory_order_acquire); }

  // TaskRunner:
  bool RunsTasksInCurrentSequence() const override;
  size_t GetPendingTaskCount() const override;

  // ---- Test support, used by base::test::TaskEnvironment ------------------
  // Runs one task whose ready_time <= |now|, inline on the calling thread.
  // Returns false when nothing is ready.
  bool RunNextReadyTask(TimeTicks now);
  // Runs every task ready at |now| plus everything those post in turn, until
  // nothing ready remains. Bounded by |max_tasks| so that an accidental
  // self-reposting loop fails the test instead of hanging it.
  size_t RunAllReadyTasks(TimeTicks now, size_t max_tasks = 100000);
  // Earliest ready_time among pending tasks, or a null TimeTicks when empty.
  TimeTicks GetNextReadyTime() const;
  // Drops every pending task. This is also how a caller breaks a reference
  // cycle when a queued task holds the last scoped_refptr to this queue.
  void Clear();
  // Makes PostDelayedTask() compute ready times against |clock| instead of the
  // real clock. TaskEnvironment::MOCK_TIME needs this, otherwise a task posted
  // with a delay under virtual time would land far in the future relative to
  // the virtual clock. Must be called before any task is posted.
  void SetTickClockForTesting(const TickClock* clock);
  // Marks the calling thread as this queue's sequence without entering Run().
  // Used by TaskEnvironment, which drives tasks inline on the test thread.
  void BindToCurrentSequence();

 protected:
  bool PostDelayedTaskImpl(const Location& from_here, OnceClosure task,
                           TimeDelta delay) override;

 private:
  friend class RefCountedThreadSafe<TaskRunner>;

  struct Task {
    TimeTicks ready_time;
    int64_t sequence_number{0};
    Location from_here;
    OnceClosure closure;

    bool operator<(const Task& o) const {
      if (ready_time != o.ready_time) {
        return ready_time < o.ready_time;
      }
      return sequence_number < o.sequence_number;
    }
  };

  mutable Lock lock_;
  ConditionVariable cv_;
  const TickClock* tick_clock_{nullptr};   // Test-only injection; see above.
  // Kept sorted; the queue is short enough that binary-search insertion beats
  // maintaining two heaps here.
  std::vector<Task> tasks_ GUARDED_BY(lock_);
  std::atomic<int64_t> next_sequence_{0};
  std::atomic<bool> quit_{false};
  mutable std::atomic<std::thread::id> running_thread_{};
};

}  // namespace ijkpp::base

#endif  // IJKPP_BASE_TASK_TASK_QUEUE_H_
