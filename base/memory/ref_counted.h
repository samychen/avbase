// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/memory/ref_counted.h` (BSD-3-Clause).

#ifndef IJKPP_BASE_MEMORY_REF_COUNTED_H_
#define IJKPP_BASE_MEMORY_REF_COUNTED_H_

#include <atomic>
#include <utility>

#include "base/check.h"
#include "base/memory/scoped_refptr.h"

namespace ijkpp::base {

// Declaring this inside a class forbids accidental construction on the stack
// or via `new T` without adoption by a scoped_refptr. Mirrors Chromium's
// REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE().
#define REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE()                  \
  static_assert(sizeof(::ijkpp::base::subtle::AdoptionHelper) > 0, "");  \
  friend class ::ijkpp::base::subtle::AdoptionHelper

namespace subtle {

class AdoptionHelper {
 public:
  template <typename T>
  static void CheckUse(const T* obj) {
    CHECK(obj);
  }
};

class RefCountedBase {
 public:
  void AddRef() const {
    DCHECK_GE(ref_count_.fetch_add(1, std::memory_order_relaxed), 0);
  }

 protected:
  RefCountedBase() = default;
  ~RefCountedBase() {
    DCHECK_EQ(ref_count_.load(std::memory_order_relaxed), 0);
  }
  // Returns true when this Release() dropped the last reference.
  bool ReleaseImpl() const {
    return ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1;
  }
  int ref_count() const { return ref_count_.load(std::memory_order_relaxed); }

 private:
  mutable std::atomic<int> ref_count_{0};
};

class RefCountedThreadSafeBase {
 public:
  void AddRef() const {
    DCHECK_GE(ref_count_.fetch_add(1, std::memory_order_relaxed), 0);
  }
  bool HasOneRef() const {
    return ref_count_.load(std::memory_order_acquire) == 1;
  }

 protected:
  RefCountedThreadSafeBase() = default;
  ~RefCountedThreadSafeBase() {
    DCHECK_EQ(ref_count_.load(std::memory_order_relaxed), 0);
  }
  bool ReleaseImpl() const {
    return ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1;
  }
  int ref_count() const { return ref_count_.load(std::memory_order_relaxed); }

 private:
  mutable std::atomic<int> ref_count_{0};
};

}  // namespace subtle

// Single-sequence ref counting. No atomic overhead on AddRef/Release.
template <typename T>
class RefCounted : public subtle::RefCountedBase {
 public:
  RefCounted(const RefCounted&) = delete;
  RefCounted& operator=(const RefCounted&) = delete;

  void Release() const {
    if (ReleaseImpl()) {
      delete static_cast<const T*>(this);
    }
  }

 protected:
  RefCounted() = default;
  ~RefCounted() = default;
};

// Thread-safe ref counting. Use for objects that cross sequences, which in
// ijkpp means every media::DecoderBuffer and media::VideoFrame.
//
// RULE: the destructor here is deliberately NON-virtual, matching Chromium.
// Release() does `delete static_cast<const T*>(this)`, where T is the complete
// most-derived type, so no virtual dispatch is needed. Consequences for a
// class deriving from this:
//   * if the class has no other virtual members, declare `~Foo();` — writing
//     `~Foo() override;` is a compile error ("does not override");
//   * if the class DOES have virtual members (e.g. media::AudioRendererSink),
//     declare `virtual ~Foo() = default;` yourself so the delete dispatches
//     correctly and -Wdelete-non-virtual-dtor stays quiet.
template <typename T>
class RefCountedThreadSafe : public subtle::RefCountedThreadSafeBase {
 public:
  RefCountedThreadSafe(const RefCountedThreadSafe&) = delete;
  RefCountedThreadSafe& operator=(const RefCountedThreadSafe&) = delete;

  void Release() const {
    if (ReleaseImpl()) {
      delete static_cast<const T*>(this);
    }
  }

 protected:
  RefCountedThreadSafe() = default;
  ~RefCountedThreadSafe() = default;
};

}  // namespace ijkpp::base

#endif  // IJKPP_BASE_MEMORY_REF_COUNTED_H_
