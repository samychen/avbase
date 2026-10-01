// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `base/task/sequenced_task_runner.h` (BSD-3-Clause).

#ifndef AVBASE_BASE_TASK_SEQUENCED_TASK_RUNNER_H_
#define AVBASE_BASE_TASK_SEQUENCED_TASK_RUNNER_H_

#include "base/base_export.h"
#include "base/task/task_runner.h"

namespace avbase::base {

// A TaskRunner whose tasks run one at a time in posting order.
//
// "Sequence" in avbase means exactly this: a serialised execution context. Most
// sequences are backed by a dedicated thread (base::Thread), but a sequence is
// not a thread — base::test::TaskEnvironment runs one inline on the test
// thread, which is what makes scheduling logic deterministically testable.
class AVBASE_BASE_EXPORT SequencedTaskRunner : public TaskRunner {
 public:
  // The default runner for the calling sequence, or nullptr when it has none.
  // Set automatically by base::Thread and base::test::TaskEnvironment.
  static scoped_refptr<SequencedTaskRunner> GetCurrentDefault();
  static void SetCurrentDefault(scoped_refptr<SequencedTaskRunner> runner);
  static bool HasCurrentDefault();

 protected:
  SequencedTaskRunner();
  ~SequencedTaskRunner() override;
};

// Convenience alias matching Chromium usage where a sequence is pinned to one
// thread. avbase does not distinguish the two at the type level: every sequence
// here is single-threaded by construction.
using SingleThreadTaskRunner = SequencedTaskRunner;

}  // namespace avbase::base

#endif  // AVBASE_BASE_TASK_SEQUENCED_TASK_RUNNER_H_
