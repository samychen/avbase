// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/memory/scoped_refptr.h` (BSD-3-Clause).
//
// ijkpp uses scoped_refptr (not std::shared_ptr) for ref-counted media objects
// such as media::DecoderBuffer and media::VideoFrame, matching Chromium. It is
// smaller (no weak count), has no exception paths, and interoperates with the
// REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE() guard.
//
// The three free functions at the bottom of this header are the whole ownership
// vocabulary, and picking the wrong one is a lifetime bug rather than a style
// problem: MakeRefCounted creates, WrapRefCounted shares, AdoptRef takes over.
// They live here and not in base/memory/ptr_util.h because two of them need
// scoped_refptr's adopting constructor, and ptr_util.h deliberately does not
// include this header.

#ifndef IJKPP_BASE_MEMORY_SCOPED_REFPTR_H_
#define IJKPP_BASE_MEMORY_SCOPED_REFPTR_H_

#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

#include "base/check.h"

namespace ijkpp::base {

template <typename T>
class scoped_refptr {
 public:
  using element_type = T;

  constexpr scoped_refptr() noexcept = default;
  constexpr scoped_refptr(std::nullptr_t) noexcept {}

  constexpr scoped_refptr(T* p) noexcept : ptr_(p) { AddRef(); }

  constexpr scoped_refptr(const scoped_refptr& o) noexcept : ptr_(o.ptr_) { AddRef(); }
  template <typename U>
  constexpr scoped_refptr(const scoped_refptr<U>& o) noexcept
      requires(std::is_convertible_v<U*, T*>)
      : ptr_(o.get()) { AddRef(); }

  constexpr scoped_refptr(scoped_refptr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
  template <typename U>
  constexpr scoped_refptr(scoped_refptr<U>&& o) noexcept
      requires(std::is_convertible_v<U*, T*>)
      : ptr_(o.release()) {}

  ~scoped_refptr() { Release(); }

  constexpr scoped_refptr& operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }
  constexpr scoped_refptr& operator=(T* p) noexcept {
    if (ptr_ != p) {
      scoped_refptr<T> tmp(p);
      std::swap(ptr_, tmp.ptr_);
    }
    return *this;
  }
  constexpr scoped_refptr& operator=(const scoped_refptr& o) noexcept {
    return *this = o.ptr_;
  }
  template <typename U>
  constexpr scoped_refptr& operator=(const scoped_refptr<U>& o) noexcept
      requires(std::is_convertible_v<U*, T*>) {
    return *this = o.get();
  }
  constexpr scoped_refptr& operator=(scoped_refptr&& o) noexcept {
    Release();
    ptr_ = o.ptr_;
    o.ptr_ = nullptr;
    return *this;
  }
  template <typename U>
  constexpr scoped_refptr& operator=(scoped_refptr<U>&& o) noexcept
      requires(std::is_convertible_v<U*, T*>) {
    Release();
    ptr_ = o.release();
    return *this;
  }

  constexpr T* get() const noexcept { return ptr_; }
  constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }
  constexpr T& operator*() const noexcept {
    CHECK(ptr_);
    return *ptr_;
  }
  constexpr T* operator->() const noexcept {
    CHECK(ptr_);
    return ptr_;
  }

  constexpr T* release() noexcept {
    T* p = ptr_;
    ptr_ = nullptr;
    return p;
  }
  constexpr void reset() noexcept { Release(); ptr_ = nullptr; }
  constexpr void swap(scoped_refptr& o) noexcept { std::swap(ptr_, o.ptr_); }

  constexpr friend bool operator==(const scoped_refptr& a, const scoped_refptr& b) noexcept {
    return a.ptr_ == b.ptr_;
  }
  constexpr friend bool operator==(const scoped_refptr& a, std::nullptr_t) noexcept {
    return a.ptr_ == nullptr;
  }
  constexpr friend bool operator==(std::nullptr_t, const scoped_refptr& a) noexcept {
    return a.ptr_ == nullptr;
  }
  constexpr friend auto operator<=>(const scoped_refptr& a, const scoped_refptr& b) noexcept {
    return a.ptr_ <=> b.ptr_;
  }

  // Takes ownership of a reference the caller already holds, WITHOUT adding
  // one. Public because the tag type below is unnameable outside base/, which
  // is what keeps this from becoming a second, silent way to adopt.
  struct AdoptTag {};
  constexpr scoped_refptr(T* p, AdoptTag) noexcept : ptr_(p) {}

 private:
  constexpr void AddRef() const noexcept {
    if (ptr_) ptr_->AddRef();
  }
  void Release() const {
    if (ptr_) ptr_->Release();
  }

  T* ptr_ = nullptr;
};

// Creates a new ref-counted object held by exactly one reference.
//
// The `new T(...)` here is the ONLY place in base/ that constructs a
// ref-counted object outside its own translation unit, so T's constructor must
// be reachable from this function. That is why media::MediaLog declares
// `MediaLog();` public while keeping `~MediaLog()` private with
// `friend class base::RefCountedThreadSafe<MediaLog>`: Release() needs the
// destructor, MakeRefCounted needs the constructor, and neither needs the
// other. A class that wants to forbid direct construction makes its
// constructor protected too -- media::DataSource does -- at the cost that only
// a derived class with a public constructor (MemoryDataSource, and the test
// fakes) can be instantiated this way.
//
// Refcount arithmetic: RefCountedBase starts at 0, the scoped_refptr(T*)
// constructor adds one, Release() subtracts one and deletes at 0, and
// ~RefCountedBase DCHECKs that the count is back to 0. Adopting instead of
// adding here would leave the count at 0 while a scoped_refptr holds it, and
// the first Release() would take it to -1.
template <typename T, typename... Args>
scoped_refptr<T> MakeRefCounted(Args&&... args) {
  return scoped_refptr<T>(new T(std::forward<Args>(args)...));
}

// Takes over a reference the caller already holds, without adding one.
//
//   T* raw = new T;            // caller holds the only reference
//   auto p = AdoptRef(raw);    // p owns it; count is still 1, not 2
//
// THIS USED TO BE WRONG. It was spelled `return scoped_refptr<T>(p);`, which
// adds a reference -- identical to WrapRefCounted below -- so the caller's
// original reference was leaked and the object was never deleted (in a debug
// build, ~RefCountedBase's DCHECK_EQ(count, 0) would fire first). Nothing
// called it, which is the only reason the bug was latent rather than live; a
// grep for AdoptRef across the tree returned its own definition and nothing
// else. Fixed before M9's RetryDataSource becomes the first real caller.
template <typename T>
constexpr scoped_refptr<T> AdoptRef(T* p) noexcept {
  return scoped_refptr<T>(p, typename scoped_refptr<T>::AdoptTag{});
}

// Adds a reference to an object that is already ref-counted and owned by
// someone else.
//
//   void SetInner(base::scoped_refptr<DataSource> inner);   // owns a ref
//   base::scoped_refptr<DataSource> inner_;
//   ...
//   auto alias = base::WrapRefCounted(inner_.get());        // second owner
//
// This is the spelling to use when a raw pointer arrives from an API that does
// not transfer ownership -- a Demuxer stream, a decorator's inner source, a
// callback argument -- and the result must share the object's lifetime rather
// than steal it. `scoped_refptr<T>(p)` does exactly the same thing; this name
// exists because at a call site "wrap" and "adopt" are one character apart in
// meaning and a lifetime apart in consequence, and Chromium pays for the
// explicit name for the same reason.
//
// Passing nullptr is fine and yields a null scoped_refptr.
template <typename T>
constexpr scoped_refptr<T> WrapRefCounted(T* p) noexcept {
  return scoped_refptr<T>(p);
}

// Overload for the common case of re-wrapping something a scoped_refptr
// already owns, so callers do not have to write .get() and think about whether
// the pointer could be null.
template <typename T>
constexpr scoped_refptr<T> WrapRefCounted(const scoped_refptr<T>& p) noexcept {
  return p;
}

}  // namespace ijkpp::base

namespace std {
template <typename T>
struct hash<::ijkpp::base::scoped_refptr<T>> {
  size_t operator()(const ::ijkpp::base::scoped_refptr<T>& p) const noexcept {
    return hash<T*>()(p.get());
  }
};
}  // namespace std

#endif  // IJKPP_BASE_MEMORY_SCOPED_REFPTR_H_
