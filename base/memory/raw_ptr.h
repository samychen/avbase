// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/memory/raw_ptr.h` (BSD-3-Clause).
// This is the annotation-only form: raw_ptr<T> behaves exactly like T* but
// marks "non-owning" intent, and leaves room to swap in BackupRefPtr later
// without touching call sites.

#ifndef IJKPP_BASE_MEMORY_RAW_PTR_H_
#define IJKPP_BASE_MEMORY_RAW_PTR_H_

#include <memory>
#include <type_traits>

namespace ijkpp::base {

template <typename T>
class raw_ptr {
 public:
  using element_type = T;

  constexpr raw_ptr() noexcept = default;
  constexpr raw_ptr(std::nullptr_t) noexcept {}
  constexpr raw_ptr(T* p) noexcept : ptr_(p) {}
  constexpr raw_ptr(const raw_ptr&) noexcept = default;
  constexpr raw_ptr(raw_ptr&&) noexcept = default;
  template <typename U>
  constexpr raw_ptr(const raw_ptr<U>& o) noexcept
      requires(std::is_convertible_v<U*, T*>)
      : ptr_(o.get()) {}

  constexpr raw_ptr& operator=(T* p) noexcept { ptr_ = p; return *this; }
  constexpr raw_ptr& operator=(const raw_ptr&) noexcept = default;
  constexpr raw_ptr& operator=(raw_ptr&&) noexcept = default;
  constexpr raw_ptr& operator=(std::nullptr_t) noexcept { ptr_ = nullptr; return *this; }

  constexpr T* get() const noexcept { return ptr_; }
  constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }
  constexpr T& operator*() const noexcept { return *ptr_; }
  constexpr T* operator->() const noexcept { return ptr_; }
  constexpr operator T*() const noexcept { return ptr_; }

  constexpr void reset() noexcept { ptr_ = nullptr; }
  constexpr void swap(raw_ptr& o) noexcept { std::swap(ptr_, o.ptr_); }

  constexpr friend bool operator==(const raw_ptr& a, const raw_ptr& b) noexcept {
    return a.ptr_ == b.ptr_;
  }
  constexpr friend bool operator==(const raw_ptr& a, T* b) noexcept {
    return a.ptr_ == b;
  }
  constexpr friend bool operator==(T* a, const raw_ptr& b) noexcept {
    return a == b.ptr_;
  }

 private:
  T* ptr_ = nullptr;
};

template <typename T>
class raw_ref {
 public:
  explicit constexpr raw_ref(T& r) noexcept : ptr_(&r) {}
  constexpr T& get() const noexcept { return *ptr_; }
  constexpr T& operator*() const noexcept { return *ptr_; }
  constexpr T* operator->() const noexcept { return ptr_; }

 private:
  T* ptr_;
};

}  // namespace ijkpp::base

#endif  // IJKPP_BASE_MEMORY_RAW_PTR_H_
