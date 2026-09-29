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

 private:
  constexpr void AddRef() const noexcept {
    if (ptr_) ptr_->AddRef();
  }
  void Release() const {
    if (ptr_) ptr_->Release();
  }

  T* ptr_ = nullptr;
};

template <typename T, typename... Args>
scoped_refptr<T> MakeRefCounted(Args&&... args) {
  return scoped_refptr<T>(new T(std::forward<Args>(args)...));
}

template <typename T>
scoped_refptr<T> AdoptRef(T* p) {
  return scoped_refptr<T>(p);
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
