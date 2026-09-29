// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `base/task/task_runner.h` (BSD-3-Clause).

#ifndef IJKPP_BASE_TASK_TASK_RUNNER_H_
#define IJKPP_BASE_TASK_TASK_RUNNER_H_

#include <cstddef>

#include "base/base_export.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"

namespace ijkpp::base {

// A destination for posted tasks.
//
// Posting is the ONLY sanctioned way to touch an object owned by another
// sequence in ijkpp. Combined with base::WeakPtr it structurally eliminates
// the "player released while a callback is in flight" crash class that
// ijkplayer cannot prevent (docs/04 §7 R11).
//
// PostTask never blocks and never runs the task inline.
class IJKPP_BASE_EXPORT TaskRunner : public RefCountedThreadSafe<TaskRunner> {
 public:
  TaskRunner(const TaskRunner&) = delete;
  TaskRunner& operator=(const TaskRunner&) = delete;

  bool PostTask(const Location& from_here, OnceClosure task);
  bool PostDelayedTask(const Location& from_here, OnceClosure task,
                       TimeDelta delay);

  // True when called on a sequence this runner posts to.
  virtual bool RunsTasksInCurrentSequence() const = 0;

  // Number of tasks accepted but not yet run. Diagnostic only.
  virtual size_t GetPendingTaskCount() const = 0;

 protected:
  TaskRunner();
  // Virtual because TaskRunner has virtual members: RefCountedThreadSafe
  // deletes through `static_cast<const T*>(this)`, so the destructor must
  // dispatch virtually or the derived part is never destroyed.
  virtual ~TaskRunner();

  // Returns false when the runner has been shut down and the task is dropped.
  // Implementations must never run |task| inline.
  virtual bool PostDelayedTaskImpl(const Location& from_here, OnceClosure task,
                                   TimeDelta delay) = 0;

 private:
  friend class RefCountedThreadSafe<TaskRunner>;
};

}  // namespace ijkpp::base

#endif  // IJKPP_BASE_TASK_TASK_RUNNER_H_
