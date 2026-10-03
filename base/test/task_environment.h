// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `base/test/task_environment.h` (BSD-3-Clause).
//
// This is what makes avbase's scheduling logic testable at all. ijkplayer's
// video_refresh() reads av_gettime_relative() directly and mutates a
// 200-field god struct, so the only way to exercise it is to play a real file
// in real time and watch. Here a test constructs a TaskEnvironment with
// MOCK_TIME, posts tasks, and advances the clock by hand — no thread, no
// sleep, no flakiness, and the whole media/filters suite runs in under a
// second.

#ifndef AVBASE_BASE_TEST_TASK_ENVIRONMENT_H_
#define AVBASE_BASE_TEST_TASK_ENVIRONMENT_H_

#include <cstddef>
#include <memory>

#include "base/base_export.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/task_queue.h"
#include "base/time/simple_test_tick_clock.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"

namespace avbase::base::test {

class AVBASE_BASE_EXPORT TaskEnvironment {
 public:
  enum class TimeSource {
    kMockTime,  // Default: time only moves when the test moves it.
    kRealTime,  // For the rare test that must observe real elapsed time.
  };

  explicit TaskEnvironment(TimeSource time_source = TimeSource::kMockTime);
  TaskEnvironment(const TaskEnvironment&) = delete;
  TaskEnvironment& operator=(const TaskEnvironment&) = delete;
  // Runs any remaining tasks, then restores the previous default runner.
  ~TaskEnvironment();

  // The runner for the sequence the test itself is on. Post to it, then call
  // RunUntilIdle().
  SequencedTaskRunner* GetMainThreadTaskRunner() const;
  // Owning reference, for APIs that take a scoped_refptr (media::Demuxer).
  base::scoped_refptr<SequencedTaskRunner> GetMainThreadTaskRunnerRef() const;

  // Inject this into the code under test in place of DefaultTickClock.
  const TickClock* GetTickClock() const;
  TimeTicks NowTicks() const;

  // Runs every task that is ready now, plus everything those post in turn.
  // Delayed tasks scheduled in the future are NOT run.
  void RunUntilIdle();

  // Advances the mock clock to |now + delta|, running every task that becomes
  // due along the way in ready-time order. No-ops the clock advance under
  // kRealTime (real time cannot be fast-forwarded) but still drains the queue.
  void FastForwardBy(TimeDelta delta);

  // Moves the clock without running anything.
  void AdvanceClock(TimeDelta delta);

  size_t GetPendingTaskCount() const;
  // True when a task is queued that no RunUntilIdle() would run because it is
  // scheduled in the future. Useful for asserting "nothing is pending".
  bool HasDelayedTasks() const;

 private:
  const TimeSource time_source_;
  std::unique_ptr<SimpleTestTickClock> mock_clock_;
  scoped_refptr<TaskQueue> queue_;
  scoped_refptr<SequencedTaskRunner> previous_default_;
};

}  // namespace avbase::base::test

#endif  // AVBASE_BASE_TEST_TASK_ENVIRONMENT_H_
