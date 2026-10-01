// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/synchronization/waitable_event.h`
// (BSD-3-Clause).

#ifndef AVBASE_BASE_SYNCHRONIZATION_WAITABLE_EVENT_H_
#define AVBASE_BASE_SYNCHRONIZATION_WAITABLE_EVENT_H_

#include <condition_variable>
#include <mutex>

#include "base/time/time.h"

namespace avbase::base {

// A manual- or automatic-reset event. avbase uses this for bounded shutdown
// waits so that ~Player() can never block indefinitely (see docs/04 §5.4 and
// behaviour difference Δ15).
class WaitableEvent {
 public:
  enum class ResetPolicy { kManualReset, kAutomaticReset };
  enum class InitialState { kSignaled, kNotSignaled };

  WaitableEvent(ResetPolicy reset_policy = ResetPolicy::kManualReset,
                InitialState initial_state = InitialState::kNotSignaled)
      : manual_reset_(reset_policy == ResetPolicy::kManualReset),
        signaled_(initial_state == InitialState::kSignaled) {}
  WaitableEvent(const WaitableEvent&) = delete;
  WaitableEvent& operator=(const WaitableEvent&) = delete;
  // The caller must guarantee no thread is inside Signal()/Wait() when this
  // runs. base::Thread::Stop() satisfies that by joining before its members
  // are destroyed; tests must do the same.
  ~WaitableEvent() = default;

  // Notifies *while still holding the mutex*. Releasing first would open a
  // use-after-free window: a waiter could return from TimedWait() and destroy
  // this WaitableEvent while notify_all() is still running inside it.
  // ThreadSanitizer catches that, and it is a real bug, not a test artifact.
  void Signal() {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = true;
    cv_.notify_all();
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    signaled_ = false;
  }

  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return signaled_; });
    if (!manual_reset_) {
      signaled_ = false;
    }
  }

  // Returns true when the event became signaled, false on timeout.
  bool TimedWait(TimeDelta timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ok = cv_.wait_for(lock, timeout.ToChronoMicros(),
                                 [this] { return signaled_; });
    if (ok && !manual_reset_) {
      signaled_ = false;
    }
    return ok;
  }

  bool IsSignaled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return signaled_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  const bool manual_reset_;
  bool signaled_;
};

}  // namespace avbase::base

#endif  // AVBASE_BASE_SYNCHRONIZATION_WAITABLE_EVENT_H_
