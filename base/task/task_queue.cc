// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task/task_queue.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "base/time/default_tick_clock.h"

namespace avbase::base {

TaskQueue::TaskQueue() : cv_(&lock_) {}

TaskQueue::~TaskQueue() {
  // Anything still queued is dropped here. A task holding a WeakPtr is already
  // inert; a task holding a OnceClosure that owns resources releases them.
  AutoLock scoped(lock_);
  tasks_.clear();
}

bool TaskQueue::PostDelayedTaskImpl(const Location& from_here, OnceClosure task,
                                    TimeDelta delay) {
  if (quit_.load(std::memory_order_acquire)) {
    // The sequence is gone. Dropping the task is correct: it almost certainly
    // holds a WeakPtr that is already invalid, and running it would resurrect
    // work against a destroyed object. The lock-free check here is a fast path;
    // the authoritative re-check happens under lock_ below.
    return false;
  }

  const TimeTicks ready_time =
      (tick_clock_ ? tick_clock_->NowTicks() : TimeTicks::Now()) + delay;

  Task pending;
  pending.ready_time = ready_time;
  pending.sequence_number = next_sequence_.fetch_add(1, std::memory_order_relaxed);
  pending.from_here = from_here;
  pending.closure = std::move(task);

  bool wake_now = false;
  {
    AutoLock scoped(lock_);
    if (quit_) {
      return false;
    }
    // Insert keeping the (ready_time, sequence_number) ordering invariant.
    // upper_bound must use the SAME comparator the range is sorted by; passing
    // a reversed one silently inserted each new task at the front, which ran a
    // reply before the task that produces its value. Caught by
    // TaskRunnerUtilTest.PostTaskAndReplyWithResultDeliversTheValue.
    auto it = std::upper_bound(tasks_.begin(), tasks_.end(), pending,
                               [](const Task& a, const Task& b) { return a < b; });
    wake_now = (it == tasks_.begin());
    tasks_.insert(it, std::move(pending));
  }
  if (wake_now) {
    // Only the new front of the queue can shorten the current wait.
    cv_.Broadcast();
  }
  return true;
}

void TaskQueue::Run() {
  BindToCurrentSequence();
  scoped_refptr<SequencedTaskRunner> previous =
      SequencedTaskRunner::GetCurrentDefault();
  SequencedTaskRunner::SetCurrentDefault(this);

  for (;;) {
    Task task;
    {
      AutoLock scoped(lock_);
      // Checked under the lock, paired with Quit() setting it under the lock.
      // Checking outside would reopen the lost-wakeup window.
      if (quit_.load(std::memory_order_acquire)) {
        break;
      }
      const TimeTicks now = TimeTicks::Now();
      if (tasks_.empty()) {
        cv_.Wait();
        continue;
      }
      if (tasks_.front().ready_time > now) {
        // TimedWait releases and re-acquires the lock around the wait.
        cv_.TimedWait(tasks_.front().ready_time - now);
        continue;
      }
      task = std::move(tasks_.front());
      tasks_.erase(tasks_.begin());
    }
    // Run outside the lock: a task may post more work, and holding the lock
    // across user code would serialise the whole player.
    std::move(task.closure).Run();
  }

  SequencedTaskRunner::SetCurrentDefault(std::move(previous));
  running_thread_.store(std::thread::id{}, std::memory_order_release);
}

void TaskQueue::Quit() {
  // The flag must be set *while holding the lock*. Signalling without it is a
  // lost wakeup: Run() can observe quit_==false, then block in cv_.Wait() after
  // this broadcast has already been delivered, and never wake again. That hang
  // is exactly ijkplayer's "release() blocks forever" class of bug (docs/01
  // 病灶 11), and LeakSanitizer/ASan's slower timing is what exposed it here.
  {
    AutoLock scoped(lock_);
    quit_.store(true, std::memory_order_release);
  }
  cv_.Broadcast();
}

void TaskQueue::BindToCurrentSequence() {
  running_thread_.store(std::this_thread::get_id(), std::memory_order_release);
}

bool TaskQueue::RunsTasksInCurrentSequence() const {
  const std::thread::id bound = running_thread_.load(std::memory_order_acquire);
  return bound != std::thread::id() && bound == std::this_thread::get_id();
}

size_t TaskQueue::GetPendingTaskCount() const {
  AutoLock scoped(lock_);
  return tasks_.size();
}

bool TaskQueue::RunNextReadyTask(TimeTicks now) {
  Task task;
  {
    AutoLock scoped(lock_);
    if (tasks_.empty() || tasks_.front().ready_time > now) {
      return false;
    }
    task = std::move(tasks_.front());
    tasks_.erase(tasks_.begin());
  }
  std::move(task.closure).Run();
  return true;
}

size_t TaskQueue::RunAllReadyTasks(TimeTicks now, size_t max_tasks) {
  size_t ran = 0;
  while (ran < max_tasks && RunNextReadyTask(now)) {
    ++ran;
  }
  if (ran == max_tasks) {
    LOG(ERROR) << "TaskQueue::RunAllReadyTasks hit the " << max_tasks
               << " task bound; a task is probably reposting itself forever.";
  }
  return ran;
}

TimeTicks TaskQueue::GetNextReadyTime() const {
  AutoLock scoped(lock_);
  return tasks_.empty() ? TimeTicks() : tasks_.front().ready_time;
}

void TaskQueue::Clear() {
  AutoLock scoped(lock_);
  tasks_.clear();
}

void TaskQueue::SetTickClockForTesting(const TickClock* clock) {
  tick_clock_ = clock;
}

}  // namespace avbase::base
