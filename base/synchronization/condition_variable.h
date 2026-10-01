// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_BASE_SYNCHRONIZATION_CONDITION_VARIABLE_H_
#define AVBASE_BASE_SYNCHRONIZATION_CONDITION_VARIABLE_H_

#include <condition_variable>

#include "base/memory/raw_ptr.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"

namespace avbase::base {

// ConditionVariable pairs with a base::Lock, not a raw std::mutex, so the
// lock-order checker still sees it.
class ConditionVariable {
 public:
  explicit ConditionVariable(Lock* lock) : lock_(lock) {}
  ConditionVariable(const ConditionVariable&) = delete;
  ConditionVariable& operator=(const ConditionVariable&) = delete;
  ~ConditionVariable() = default;

  // The caller must already hold the associated Lock; this adopts it for the
  // duration of the wait and releases it again on return, which is what
  // std::condition_variable expects.
  void Wait() {
    std::unique_lock<std::mutex> lock(lock_->native(), std::adopt_lock);
    cv_.wait(lock);
    lock.release();   // Caller still owns the lock.
  }

  // Returns false on timeout.
  bool TimedWait(TimeDelta max_wait) {
    std::unique_lock<std::mutex> lock(lock_->native(), std::adopt_lock);
    const bool ok = cv_.wait_for(lock, max_wait.ToChronoMicros()) ==
                    std::cv_status::no_timeout;
    lock.release();   // Caller still owns the lock.
    return ok;
  }

  void Signal() { cv_.notify_one(); }
  void Broadcast() { cv_.notify_all(); }

 private:
  std::condition_variable cv_;
  raw_ptr<Lock> lock_;
};

}  // namespace avbase::base

#endif  // AVBASE_BASE_SYNCHRONIZATION_CONDITION_VARIABLE_H_
