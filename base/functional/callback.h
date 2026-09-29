// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/functional/callback.h` (BSD-3-Clause).
//
// OnceCallback is move-only and can be Run() exactly once; RepeatingCallback
// is copyable and can be Run() many times. Neither allocates on Run().
// Neither throws: ijkpp is built with -fno-exceptions.

#ifndef IJKPP_BASE_FUNCTIONAL_CALLBACK_H_
#define IJKPP_BASE_FUNCTIONAL_CALLBACK_H_

#include <memory>
#include <utility>

#include "base/check.h"
#include "base/functional/callback_forward.h"

namespace ijkpp::base {
namespace internal {

// Type-erased single-shot invocable.
template <typename R, typename... Args>
class OnceInvoker {
 public:
  virtual ~OnceInvoker() = default;
  virtual R Invoke(Args... args) = 0;
};

template <typename R, typename... Args>
class RepeatingInvoker {
 public:
  virtual ~RepeatingInvoker() = default;
  virtual R Invoke(Args... args) = 0;
  virtual std::unique_ptr<RepeatingInvoker> Clone() const = 0;
};

template <typename Functor, typename R, typename... Args>
class OnceInvokerImpl final : public OnceInvoker<R, Args...> {
 public:
  explicit OnceInvokerImpl(Functor f) : functor_(std::move(f)) {}
  R Invoke(Args... args) override {
    return functor_(std::forward<Args>(args)...);
  }

 private:
  Functor functor_;
};

template <typename Functor, typename R, typename... Args>
class RepeatingInvokerImpl final : public RepeatingInvoker<R, Args...> {
 public:
  explicit RepeatingInvokerImpl(Functor f) : functor_(std::move(f)) {}
  R Invoke(Args... args) override {
    return functor_(std::forward<Args>(args)...);
  }
  std::unique_ptr<RepeatingInvoker<R, Args...>> Clone() const override {
    return std::make_unique<RepeatingInvokerImpl>(functor_);
  }

 private:
  Functor functor_;
};

}  // namespace internal

// ---------------------------------------------------------------------------
// OnceCallback
// ---------------------------------------------------------------------------
template <typename Sig>
class OnceCallback;

template <typename R, typename... Args>
class OnceCallback<R(Args...)> {
 public:
  using RunType = R(Args...);
  using Invoker = internal::OnceInvoker<R, Args...>;

  OnceCallback() = default;
  OnceCallback(std::nullptr_t) {}
  OnceCallback(OnceCallback&&) noexcept = default;
  OnceCallback& operator=(OnceCallback&&) noexcept = default;
  OnceCallback(const OnceCallback&) = delete;
  OnceCallback& operator=(const OnceCallback&) = delete;

  template <typename Functor>
  explicit OnceCallback(Functor f)
      : invoker_(std::make_unique<
                 internal::OnceInvokerImpl<Functor, R, Args...>>(std::move(f))) {}

  explicit operator bool() const noexcept { return invoker_ != nullptr; }
  bool is_null() const noexcept { return invoker_ == nullptr; }

  // Must be called on an rvalue; the callback is consumed.
  R Run(Args... args) && {
    CHECK(invoker_) << "OnceCallback::Run() called on a null callback";
    std::unique_ptr<Invoker> invoker = std::move(invoker_);
    if constexpr (std::is_void_v<R>) {
      invoker->Invoke(std::forward<Args>(args)...);
    } else {
      return invoker->Invoke(std::forward<Args>(args)...);
    }
  }

  // Consuming call operator, so std::invoke (and therefore BindOnce) can use an
  // OnceCallback as the functor: `BindOnce(std::move(cb), result)` is the shape
  // every async completion path needs. Chromium achieves the same through
  // specialisations in its InvokeHelper; an explicit && -consuming operator()
  // is simpler and keeps the once-only guarantee, because a second call finds
  // invoker_ null and fails the CHECK in Run() rather than silently misfiring.
  R operator()(Args... args) & {
    return std::move(*this).Run(std::forward<Args>(args)...);
  }

  void Reset() noexcept { invoker_.reset(); }

 private:
  std::unique_ptr<Invoker> invoker_;
};

// ---------------------------------------------------------------------------
// RepeatingCallback
// ---------------------------------------------------------------------------
template <typename Sig>
class RepeatingCallback;

template <typename R, typename... Args>
class RepeatingCallback<R(Args...)> {
 public:
  using RunType = R(Args...);
  using Invoker = internal::RepeatingInvoker<R, Args...>;

  RepeatingCallback() = default;
  RepeatingCallback(std::nullptr_t) {}
  RepeatingCallback(const RepeatingCallback&) = default;
  RepeatingCallback& operator=(const RepeatingCallback&) = default;
  RepeatingCallback(RepeatingCallback&&) noexcept = default;
  RepeatingCallback& operator=(RepeatingCallback&&) noexcept = default;

  template <typename Functor>
  explicit RepeatingCallback(Functor f)
      : invoker_(std::make_unique<
                 internal::RepeatingInvokerImpl<Functor, R, Args...>>(
            std::move(f))) {}

  explicit operator bool() const noexcept { return invoker_ != nullptr; }
  bool is_null() const noexcept { return invoker_ == nullptr; }

  R Run(Args... args) const& {
    CHECK(invoker_) << "RepeatingCallback::Run() called on a null callback";
    if constexpr (std::is_void_v<R>) {
      invoker_->Invoke(std::forward<Args>(args)...);
    } else {
      return invoker_->Invoke(std::forward<Args>(args)...);
    }
  }

  // Non-consuming call operator, so std::invoke / BindOnce can use a
  // RepeatingCallback as the functor.
  R operator()(Args... args) const { return Run(std::forward<Args>(args)...); }
  R Run(Args... args) && {
    CHECK(invoker_) << "RepeatingCallback::Run() called on a null callback";
    std::unique_ptr<Invoker> invoker = std::move(invoker_);
    if constexpr (std::is_void_v<R>) {
      invoker->Invoke(std::forward<Args>(args)...);
    } else {
      return invoker->Invoke(std::forward<Args>(args)...);
    }
  }

  void Reset() noexcept { invoker_.reset(); }

 private:
  std::shared_ptr<Invoker> invoker_;
};

}  // namespace ijkpp::base

#endif  // IJKPP_BASE_FUNCTIONAL_CALLBACK_H_
