// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/test/task_environment.h"

#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/default_tick_clock.h"

namespace ijkpp::base::test {

TaskEnvironment::TaskEnvironment(TimeSource time_source)
    : time_source_(time_source),
      queue_(MakeRefCounted<TaskQueue>()),
      previous_default_(SequencedTaskRunner::GetCurrentDefault()) {
  if (time_source_ == TimeSource::kMockTime) {
    mock_clock_ = std::make_unique<SimpleTestTickClock>();
    // Start at a non-zero instant: TimeTicks() is the null value, and a queue
    // whose tasks are all "ready at null" cannot be distinguished from an
    // uninitialised deadline.
    mock_clock_->StartAt(TimeTicks::FromMicroseconds(1000000));
    queue_->SetTickClockForTesting(mock_clock_.get());
  }
  queue_->BindToCurrentSequence();
  SequencedTaskRunner::SetCurrentDefault(queue_);
}

TaskEnvironment::~TaskEnvironment() {
  // Drain rather than abandon: a task left running after the fixture dies is
  // indistinguishable from a leak in the code under test.
  RunUntilIdle();
  queue_->Quit();
  SequencedTaskRunner::SetCurrentDefault(std::move(previous_default_));
}

SequencedTaskRunner* TaskEnvironment::GetMainThreadTaskRunner() const {
  return queue_.get();
}

base::scoped_refptr<SequencedTaskRunner>
TaskEnvironment::GetMainThreadTaskRunnerRef() const {
  return queue_;
}

const TickClock* TaskEnvironment::GetTickClock() const {
  return mock_clock_ ? static_cast<const TickClock*>(mock_clock_.get())
                     : DefaultTickClock::GetInstance();
}

TimeTicks TaskEnvironment::NowTicks() const { return GetTickClock()->NowTicks(); }

void TaskEnvironment::RunUntilIdle() {
  queue_->RunAllReadyTasks(NowTicks());
}

void TaskEnvironment::FastForwardBy(TimeDelta delta) {
  if (time_source_ != TimeSource::kMockTime) {
    // Real time cannot be fast-forwarded; drain what is due instead so the
    // call is still meaningful rather than silently doing nothing.
    LOG(WARNING) << "FastForwardBy() under kRealTime only drains due tasks";
    RunUntilIdle();
    return;
  }

  const TimeTicks target = mock_clock_->NowTicks() + delta;
  // Step to each pending delayed task's ready time in order, running whatever
  // becomes due there. Stepping rather than jumping means a chain of delayed
  // tasks observes the intermediate clock values it would see in production.
  for (;;) {
    const TimeTicks next = queue_->GetNextReadyTime();
    if (next.is_null() || next > target) {
      break;
    }
    mock_clock_->StartAt(next);
    if (queue_->RunAllReadyTasks(next) == 0) {
      break;
    }
  }
  mock_clock_->StartAt(target);
  queue_->RunAllReadyTasks(target);
}

void TaskEnvironment::AdvanceClock(TimeDelta delta) {
  CHECK(mock_clock_) << "AdvanceClock() requires TimeSource::kMockTime";
  mock_clock_->Advance(delta);
}

size_t TaskEnvironment::GetPendingTaskCount() const {
  return queue_->GetPendingTaskCount();
}

bool TaskEnvironment::HasDelayedTasks() const {
  const TimeTicks next = queue_->GetNextReadyTime();
  return !next.is_null() && next > NowTicks();
}

}  // namespace ijkpp::base::test
