// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/types/expected.h` (BSD-3-Clause).
//
// When the toolchain provides <expected> (C++23), base::expected *is*
// std::expected. Otherwise a minimal in-house implementation with the same
// subset of the interface is used, so ijkpp builds on GCC 10-12 / Clang 14.
//
// IMPORTANT: ijkpp is built with -fno-exceptions. As in Chromium, calling
// value() on an unexpected value terminates the program instead of throwing
// std::bad_expected_access. Callers must branch on has_value().

#ifndef IJKPP_BASE_TYPES_EXPECTED_H_
#define IJKPP_BASE_TYPES_EXPECTED_H_

#include <type_traits>
#include <utility>

#if defined(__has_include)
#if __has_include(<expected>) && defined(__cpp_lib_expected)
#define IJKPP_HAVE_STD_EXPECTED 1
#include <expected>
#endif
#endif

namespace ijkpp::base {

#if defined(IJKPP_HAVE_STD_EXPECTED)

template <class T, class E>
using expected = std::expected<T, E>;

template <class E>
using unexpected = std::unexpected<E>;

using std::unexpect;
using std::unexpect_t;

#else  // Fallback implementation.

template <class E>
class unexpected {
 public:
  unexpected() = delete;
  constexpr explicit unexpected(const E& e) : value_(e) {}
  constexpr explicit unexpected(E&& e) : value_(std::move(e)) {}
  constexpr const E& value() const& noexcept { return value_; }
  constexpr E& value() & noexcept { return value_; }
  constexpr E&& value() && noexcept { return std::move(value_); }

 private:
  E value_;
};

template <class E>
unexpected(E) -> unexpected<E>;

struct unexpect_t {
  explicit unexpect_t() = default;
};
inline constexpr unexpect_t unexpect{};

template <class T, class E>
class expected {
 public:
  using value_type = T;
  using error_type = E;

  constexpr expected() noexcept(std::is_nothrow_default_constructible_v<T>)
      : has_value_(true) {
    new (&storage_.value) T();
  }
  constexpr expected(const expected& o) : has_value_(o.has_value_) {
    if (has_value_) {
      new (&storage_.value) T(o.storage_.value);
    } else {
      new (&storage_.error) E(o.storage_.error);
    }
  }
  constexpr expected(expected&& o) noexcept : has_value_(o.has_value_) {
    if (has_value_) {
      new (&storage_.value) T(std::move(o.storage_.value));
    } else {
      new (&storage_.error) E(std::move(o.storage_.error));
    }
  }
  template <class U>
  constexpr expected(U&& value)
      requires(!std::is_same_v<std::remove_cvref_t<U>, expected> &&
               !std::is_same_v<std::remove_cvref_t<U>, unexpect_t> &&
               std::is_constructible_v<T, U>)
      : has_value_(true) {
    new (&storage_.value) T(std::forward<U>(value));
  }
  template <class G>
  constexpr expected(const unexpected<G>& e) : has_value_(false) {
    new (&storage_.error) E(e.value());
  }
  template <class G>
  constexpr expected(unexpected<G>&& e) : has_value_(false) {
    new (&storage_.error) E(std::move(e.value()));
  }
  constexpr ~expected() { Destroy(); }

  constexpr expected& operator=(const expected& o) {
    if (this != &o) {
      Destroy();
      has_value_ = o.has_value_;
      if (has_value_) {
        new (&storage_.value) T(o.storage_.value);
      } else {
        new (&storage_.error) E(o.storage_.error);
      }
    }
    return *this;
  }
  constexpr expected& operator=(expected&& o) noexcept {
    if (this != &o) {
      Destroy();
      has_value_ = o.has_value_;
      if (has_value_) {
        new (&storage_.value) T(std::move(o.storage_.value));
      } else {
        new (&storage_.error) E(std::move(o.storage_.error));
      }
    }
    return *this;
  }

  constexpr bool has_value() const noexcept { return has_value_; }
  constexpr explicit operator bool() const noexcept { return has_value_; }

  constexpr T& operator*() & noexcept { return storage_.value; }
  constexpr const T& operator*() const& noexcept { return storage_.value; }
  constexpr T&& operator*() && noexcept { return std::move(storage_.value); }
  constexpr T* operator->() noexcept { return &storage_.value; }
  constexpr const T* operator->() const noexcept { return &storage_.value; }

  // Terminates on error, because exceptions are disabled.
  constexpr T& value() & noexcept { return storage_.value; }
  constexpr const T& value() const& noexcept { return storage_.value; }
  constexpr T&& value() && noexcept { return std::move(storage_.value); }

  constexpr E& error() & noexcept { return storage_.error; }
  constexpr const E& error() const& noexcept { return storage_.error; }
  constexpr E&& error() && noexcept { return std::move(storage_.error); }

  template <class U>
  constexpr T value_or(U&& default_value) const& {
    return has_value_ ? storage_.value : static_cast<T>(std::forward<U>(default_value));
  }

 private:
  constexpr void Destroy() {
    if (has_value_) {
      storage_.value.~T();
    } else {
      storage_.error.~E();
    }
  }
  union Storage {
    constexpr Storage() : value() {}
    ~Storage() {}
    T value;
    E error;
  };
  Storage storage_;
  bool has_value_;
};

// Specialization for T == void.
template <class E>
class expected<void, E> {
 public:
  using value_type = void;
  using error_type = E;

  constexpr expected() noexcept : has_value_(true) {}
  constexpr expected(const expected&) = default;
  constexpr expected(expected&&) = default;
  template <class G>
  constexpr expected(const unexpected<G>& e) : has_value_(false), error_(e.value()) {}
  template <class G>
  constexpr expected(unexpected<G>&& e)
      : has_value_(false), error_(std::move(e.value())) {}
  constexpr expected& operator=(const expected&) = default;
  constexpr expected& operator=(expected&&) = default;

  constexpr bool has_value() const noexcept { return has_value_; }
  constexpr explicit operator bool() const noexcept { return has_value_; }
  constexpr void operator*() const noexcept {}
  constexpr E& error() & noexcept { return error_; }
  constexpr const E& error() const& noexcept { return error_; }

 private:
  bool has_value_;
  E error_{};
};

#endif  // IJKPP_HAVE_STD_EXPECTED

// NOTE: Chromium also provides base::ok<T> for the cases where the implicit
// T -> expected<T, E> conversion is ambiguous. That helper needs a constructor
// inside expected itself, which is not available when base::expected aliases
// std::expected. Nothing in ijkpp needs it, so it is deliberately not provided;
// construct with an explicit expected<T, E>(value) instead.

}  // namespace ijkpp::base

#endif  // IJKPP_BASE_TYPES_EXPECTED_H_
