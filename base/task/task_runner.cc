// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task/task_runner.h"

#include <utility>

namespace avbase::base {

TaskRunner::TaskRunner() = default;
TaskRunner::~TaskRunner() = default;

bool TaskRunner::PostTask(const Location& from_here, OnceClosure task) {
  return PostDelayedTask(from_here, std::move(task), TimeDelta());
}

bool TaskRunner::PostDelayedTask(const Location& from_here, OnceClosure task,
                                 TimeDelta delay) {
  if (!task) {
    return false;
  }
  // Negative delays are treated as "immediately". Callers compute delays from
  // durations that can legitimately go negative when a deadline has passed.
  return PostDelayedTaskImpl(from_here, std::move(task),
                             delay.is_negative() ? TimeDelta() : delay);
}

}  // namespace avbase::base
