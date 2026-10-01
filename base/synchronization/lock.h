// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/synchronization/lock.h` (BSD-3-Clause).

#ifndef IJKPP_BASE_SYNCHRONIZATION_LOCK_H_
#define IJKPP_BASE_SYNCHRONIZATION_LOCK_H_

#include <mutex>

#include "base/check.h"

// Clang's thread-safety analysis annotations. No-ops on GCC/MSVC, which means
// the annotations are still valuable documentation even where they are not
// enforced. CI runs one clang job with -Wthread-safety to enforce them.
#if defined(__clang__) && defined(__clang_major__) && (__clang_major__ >= 12)
#define IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(x) __attribute__((x))
#else
#define IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(x)
#endif

#define GUARDED_BY(x) IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))
#define GUARDED_BY_CONTEXT(x) \
  IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))
#define PT_GUARDED_BY(x) IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))
#define ACQUIRED_BEFORE(...) \
  IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(acquired_before(__VA_ARGS__))
#define ACQUIRED_AFTER(...) \
  IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(acquired_after(__VA_ARGS__))
#define EXCLUSIVE_LOCK_FUNCTION(...) \
  IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(exclusive_lock_function(__VA_ARGS__))
#define UNLOCK_FUNCTION(...) \
  IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(unlock_function(__VA_ARGS__))
#define LOCK_RETURNED(x) IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(lock_returned(x))
// The two that make the rest mean anything: without a CAPABILITY on Lock,
// every GUARDED_BY(lock_) in the tree is a diagnostic of its own ("attribute
// requires arguments whose type is annotated with 'capability'"), and without
// SCOPED_LOCKABLE on the RAII wrapper clang does not know the lock is held
// inside the scope -- which is why the tree used to emit 260 "requires holding
// mutex" warnings at call sites that do take the lock.
#define CAPABILITY(x) IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(capability(x))
#define SCOPED_LOCKABLE IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(scoped_lockable)
// For the "*Locked" convention the codebase uses for helpers that must be
// called with the lock already held: without it clang reads every member
// access inside such a helper as an unlocked one.
#define EXCLUSIVE_LOCKS_REQUIRED(...) \
  IJKPP_THREAD_ANNOTATION_ATTRIBUTE__(exclusive_locks_required(__VA_ARGS__))

namespace ijkpp::base {

// A thin std::mutex wrapper. The wrapper exists so that:
//   * locks can be named and audited by tools/check_invariants.py,
//   * the global lock-order rule (docs/04 §3.1) can be asserted in debug
//     builds via LockOrderChecker, and
//   * clang's GUARDED_BY annotation has a concrete type to attach to.
class CAPABILITY("mutex") Lock {
 public:
  Lock() = default;
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
  ~Lock() = default;

  void Acquire() EXCLUSIVE_LOCK_FUNCTION() { mutex_.lock(); }
  void Release() UNLOCK_FUNCTION() { mutex_.unlock(); }
  bool Try() { return mutex_.try_lock(); }

  // Exposed for std::condition_variable interop only.
  std::mutex& native() { return mutex_; }

  // Debug-build verification of the single global lock order documented in
  // docs/04 §3.1. No-op unless IJKPP_ENABLE_DCHECK is defined. Violations are
  // reported through base/logging at FATAL severity, because a lock-order
  // inversion in a media pipeline surfaces as an unreproducible hang.
  void AssertAcquiredInOrder();
  void RecordRelease();

 private:
  std::mutex mutex_;
};

class SCOPED_LOCKABLE AutoLock {
 public:
  explicit AutoLock(Lock& lock) EXCLUSIVE_LOCK_FUNCTION(lock) : lock_(lock) {
    lock_.AssertAcquiredInOrder();
    lock_.Acquire();
  }
  AutoLock(const AutoLock&) = delete;
  AutoLock& operator=(const AutoLock&) = delete;
  ~AutoLock() UNLOCK_FUNCTION() {
    lock_.Release();
    lock_.RecordRelease();
  }

 private:
  Lock& lock_;
};

class AutoUnlock {
 public:
  explicit AutoUnlock(Lock& lock) : lock_(lock) { lock_.Release(); }
  AutoUnlock(const AutoUnlock&) = delete;
  AutoUnlock& operator=(const AutoUnlock&) = delete;
  ~AutoUnlock() { lock_.Acquire(); }

 private:
  Lock& lock_;
};

}  // namespace ijkpp::base

#endif  // IJKPP_BASE_SYNCHRONIZATION_LOCK_H_
