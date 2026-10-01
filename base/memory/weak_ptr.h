// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/memory/weak_ptr.h` (BSD-3-Clause).
//
// WeakPtr is the single most important safety mechanism in avbase: every
// cross-sequence callback binds a WeakPtr, so a task that outlives its target
// becomes a no-op instead of a use-after-free. This structurally removes the
// whole class of "player released while a callback is in flight" crashes that
// ijkplayer cannot prevent (docs/04 §7 R11).

#ifndef AVBASE_BASE_MEMORY_WEAK_PTR_H_
#define AVBASE_BASE_MEMORY_WEAK_PTR_H_

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"

namespace avbase::base {
namespace internal {

// Shared liveness flag. The factory clears it on destruction; every WeakPtr
// copy observes the invalidation immediately.
class WeakReferenceFlag : public RefCountedThreadSafe<WeakReferenceFlag> {
 public:
  WeakReferenceFlag() = default;
  WeakReferenceFlag(const WeakReferenceFlag&) = delete;
  WeakReferenceFlag& operator=(const WeakReferenceFlag&) = delete;

  void Invalidate() { valid_.store(false, std::memory_order_release); }
  bool is_valid() const { return valid_.load(std::memory_order_acquire); }

  // Sequence affinity lives on the shared flag (as in Chromium's
  // WeakReference::Flag), so that WeakPtr itself stays copyable.
  bool CheckSequenceAffinity() const {
    const std::thread::id id = std::this_thread::get_id();
    std::thread::id expected{};
    if (bound_thread_.compare_exchange_strong(expected, id,
                                              std::memory_order_acq_rel)) {
      return true;   // First dereference binds the sequence.
    }
    return expected == id;
  }

 private:
  friend class base::RefCountedThreadSafe<WeakReferenceFlag>;
  ~WeakReferenceFlag() = default;
  std::atomic<bool> valid_{true};
  mutable std::atomic<std::thread::id> bound_thread_{};
};

}  // namespace internal

// A non-owning pointer that becomes null when the target is destroyed.
//
// PITFALL: when binding into a callback, pass the WeakPtr itself, never
// `weak_ptr.get()`. BindOnce only runs its liveness check on bound arguments
// whose type actually is a WeakPtr; a raw pointer compiles, looks identical,
// and silently disables the guard — the callback then runs against freed
// memory. ThreadTest.WeakPtrBoundTaskIsInertAfterOwnerDies covers this.
//
// Sequence affinity: a WeakPtr may be *created and copied* on the owning
// sequence and *dereferenced* on any single other sequence, but not both.
// Debug builds DCHECK the binding sequence on first use.
template <typename T>
class WeakPtr {
 public:
  WeakPtr() = default;
  WeakPtr(std::nullptr_t) {}

  WeakPtr(const WeakPtr& other) = default;
  WeakPtr(WeakPtr&& other) noexcept = default;
  WeakPtr& operator=(const WeakPtr& other) = default;
  WeakPtr& operator=(WeakPtr&& other) noexcept = default;

  template <typename U>
  WeakPtr(const WeakPtr<U>& other) noexcept
      requires(std::is_convertible_v<U*, T*>)
      : flag_(other.flag_), ptr_(other.ptr_) {}

  ~WeakPtr() = default;

  T& operator*() const {
    CheckValid();
    return *ptr_;
  }
  T* operator->() const {
    CheckValid();
    return ptr_;
  }
  T* get() const {
    return MaybeValid() ? ptr_ : nullptr;
  }

  explicit operator bool() const { return MaybeValid(); }

  void reset() {
    flag_ = nullptr;
    ptr_ = nullptr;
  }

  // True when the flag says the target is alive. Does not touch the sequence
  // checker, so it is safe to call from any thread for diagnostics.
  bool MaybeValid() const { return flag_ && flag_->is_valid(); }

 private:
  template <typename U>
  friend class WeakPtr;
  template <typename U>
  friend class WeakPtrFactory;
  friend class internal::WeakReferenceFlag;

  WeakPtr(scoped_refptr<internal::WeakReferenceFlag> flag, T* ptr)
      : flag_(std::move(flag)), ptr_(ptr) {}

  void CheckValid() const {
    CHECK(MaybeValid()) << "WeakPtr used after its target was destroyed";
#if defined(AVBASE_ENABLE_DCHECK)
    DCHECK(flag_->CheckSequenceAffinity())
        << "WeakPtr dereferenced on a different sequence than it was bound to";
#endif
  }

  // Storing only a flag and a raw pointer keeps WeakPtr trivially copyable,
  // which BindOnce() requires in order to store bound arguments in a tuple.
  scoped_refptr<internal::WeakReferenceFlag> flag_;
  raw_ptr<T> ptr_ = nullptr;
};

// Owns the liveness flag. MUST be the last member of the owning class, so that
// it is destroyed first and every outstanding WeakPtr is invalidated before
// the rest of the object goes away. tools/check_invariants.py rule C19 enforces
// this ordering.
template <typename T>
class WeakPtrFactory {
 public:
  explicit WeakPtrFactory(T* instance) : instance_(instance) {
    CHECK(instance_);
  }
  WeakPtrFactory(const WeakPtrFactory&) = delete;
  WeakPtrFactory& operator=(const WeakPtrFactory&) = delete;

  ~WeakPtrFactory() { InvalidateWeakPtrs(); }

  WeakPtr<T> GetWeakPtr() const {
    return WeakPtr<T>(flag_, instance_);
  }

  void InvalidateWeakPtrs() {
    if (flag_) {
      flag_->Invalidate();
      flag_ = nullptr;
    }
  }
  bool HasWeakPtrs() const { return static_cast<bool>(flag_); }

 private:
  mutable scoped_refptr<internal::WeakReferenceFlag> flag_ =
      base::MakeRefCounted<internal::WeakReferenceFlag>();
  raw_ptr<T> instance_;
};

}  // namespace avbase::base

#endif  // AVBASE_BASE_MEMORY_WEAK_PTR_H_
