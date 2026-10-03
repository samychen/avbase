// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/functional/bind.h` (BSD-3-Clause).
//
// Supported subset (the L1 tier of docs/08 §5.1 R2's fallback plan):
//   * lambdas and functors with a non-generic operator()
//   * free function pointers
//   * member function pointers with a T*, const T*, scoped_refptr<T> or
//     WeakPtr<T> receiver
//   * 0..N bound arguments
//   * Unretained(), DoNothing()
// Not (yet) supported: Owned() (it would have to own and delete the pointer,
// which nothing here does — shipping it without that semantics would silently
// leak), Passed(), IgnoreResult() as a binder, generic (auto-parameter)
// lambdas.

#ifndef AVBASE_BASE_FUNCTIONAL_BIND_H_
#define AVBASE_BASE_FUNCTIONAL_BIND_H_

#include <cstddef>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include "base/check.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"

namespace avbase::base {

// Wrappers that change how a bound argument is treated at invoke time.
template <typename T>
struct UnretainedWrapper {
  T* ptr;
};
template <typename T>
UnretainedWrapper<T> Unretained(T* p) {
  return UnretainedWrapper<T>{p};
}

// ---------------------------------------------------------------------------
namespace internal {

template <typename>
struct OperatorTraits;
template <typename R, typename C, typename... A>
struct OperatorTraits<R (C::*)(A...)> {
  using RunType = R(A...);
};
template <typename R, typename C, typename... A>
struct OperatorTraits<R (C::*)(A...) const> {
  using RunType = R(A...);
};

template <typename>
struct MemberTraits;
template <typename R, typename C, typename... A>
struct MemberTraits<R (C::*)(A...)> {
  using RunType = R(C*, A...);
};
template <typename R, typename C, typename... A>
struct MemberTraits<R (C::*)(A...) const> {
  using RunType = R(const C*, A...);
};

template <typename T>
struct CallableTraits
    : OperatorTraits<decltype(&std::remove_reference_t<T>::operator())> {};

template <typename R, typename... A>
struct CallableTraits<R(A...)> {
  using RunType = R(A...);
};
template <typename R, typename... A>
struct CallableTraits<R (*)(A...)> {
  using RunType = R(A...);
};
template <typename R, typename C, typename... A>
struct CallableTraits<R (C::*)(A...)> : MemberTraits<R (C::*)(A...)> {};
template <typename R, typename C, typename... A>
struct CallableTraits<R (C::*)(A...) const>
    : MemberTraits<R (C::*)(A...) const> {};
template <typename Sig>
struct CallableTraits<OnceCallback<Sig>> {
  using RunType = Sig;
};
template <typename Sig>
struct CallableTraits<RepeatingCallback<Sig>> {
  using RunType = Sig;
};

// Drops the first N parameters from a signature.
template <typename Sig, size_t N>
struct DropFirstN;
template <typename R, typename... A>
struct DropFirstN<R(A...), 0> {
  using type = R(A...);
};
template <typename R, typename First, typename... Rest, size_t N>
  requires(N > 0)
struct DropFirstN<R(First, Rest...), N> {
  using type = typename DropFirstN<R(Rest...), N - 1>::type;
};

template <typename Sig>
struct ReturnOf;
template <typename R, typename... A>
struct ReturnOf<R(A...)> {
  using type = R;
};

template <typename T>
struct IsWeakPtr : std::false_type {};
template <typename T>
struct IsWeakPtr<WeakPtr<T>> : std::true_type {};

// Unwraps the binding helpers and smart pointers into what the callee expects.
template <typename T>
T* UnwrapArg(UnretainedWrapper<T> w) {
  return w.ptr;
}
// The smart-pointer overloads need BOTH ref-qualifications. Bound arguments
// arrive as `std::forward<B>(...)`, i.e. as rvalues when B is a value type, and
// an rvalue binds better to the generic `T&&` forwarding overload than to a
// `const&` one — so without these the WeakPtr/scoped_refptr would be passed
// through unwrapped and the callee would fail to compile (or worse, for the
// WeakPtr case, the liveness guard would never run).
template <typename T>
T* UnwrapArg(const scoped_refptr<T>& p) {
  return p.get();
}
template <typename T>
T* UnwrapArg(scoped_refptr<T>&& p) {
  return p.get();
}
template <typename T>
T* UnwrapArg(const WeakPtr<T>& p) {
  return p.get();
}
template <typename T>
T* UnwrapArg(WeakPtr<T>&& p) {
  return p.get();
}
template <typename T>
T&& UnwrapArg(T&& v) {
  return std::forward<T>(v);
}

// Per-element check, gated by `if constexpr` so that .MaybeValid() is only
// ever instantiated for actual WeakPtr arguments.
template <typename T>
bool IsInvalidWeakPtr(const T& value) {
  if constexpr (IsWeakPtr<std::remove_cvref_t<T>>::value) {
    return !value.MaybeValid();
  } else {
    (void)value;
    return false;
  }
}

// True when any bound argument is an invalidated WeakPtr.
template <typename... B, size_t... I>
bool AnyWeakPtrInvalid(const std::tuple<B...>& bound,
                       std::index_sequence<I...>) {
  return (IsInvalidWeakPtr(std::get<I>(bound)) || ...);
}

template <typename CallbackSig, typename Functor, typename BoundTuple>
class BindState;

template <typename R, typename... Args, typename Functor, typename... B>
class BindState<R(Args...), Functor, std::tuple<B...>> {
 public:
  BindState(Functor f, std::tuple<B...> bound)
      : functor_(std::move(f)), bound_(std::move(bound)) {}
  BindState(const BindState&) = delete;
  BindState& operator=(const BindState&) = delete;
  BindState(BindState&&) noexcept = default;
  BindState& operator=(BindState&&) noexcept = default;
  ~BindState() = default;

  R operator()(Args... args) {
    constexpr size_t kN = sizeof...(B);
    if (AnyWeakPtrInvalid(bound_, std::make_index_sequence<kN>{})) {
      // The target died before the task ran. For void callbacks this is a
      // silent no-op, which is the whole point of binding a WeakPtr.
      if constexpr (!std::is_void_v<R>) {
        // The call is skipped, so the caller gets a default-constructed result.
        // Chromium does the same; the static_assert below is what makes it
        // safe.
        static_assert(std::is_default_constructible_v<R>,
                      "A callback bound to a WeakPtr must return a "
                      "default-constructible type, because the call is skipped "
                      "when the target has been destroyed.");
        return R{};
      } else {
        return;
      }
    }
    return std::apply(
        [&](B&... bound_args) -> R {
          return InvokeWith(functor_, UnwrapArg(std::forward<B>(bound_args))...,
                            std::forward<Args>(args)...);
        },
        bound_);
  }

 private:
  template <typename F, typename... All>
  static decltype(auto) InvokeWith(F& f, All&&... all) {
    return std::invoke(f, std::forward<All>(all)...);
  }

  Functor functor_;
  std::tuple<B...> bound_;
};

// Decay rule: scoped_refptr and WeakPtr receivers are stored by value so the
// bound object stays alive (scoped_refptr) or the liveness check works
// (WeakPtr). UnretainedWrapper / OwnedWrapper are stored as-is.
template <typename T>
struct StorageType {
  using type = std::remove_cvref_t<T>;
};

}  // namespace internal

// ---------------------------------------------------------------------------
// BindOnce
// ---------------------------------------------------------------------------
template <typename Functor, typename... BoundArgs>
auto BindOnce(Functor&& functor, BoundArgs&&... bound) {
  using Traits = internal::CallableTraits<std::remove_cvref_t<Functor>>;
  using RunType = typename Traits::RunType;
  using CallbackSig =
      typename internal::DropFirstN<RunType, sizeof...(BoundArgs)>::type;

  auto bound_tuple = std::make_tuple(
      static_cast<typename internal::StorageType<BoundArgs>::type>(
          std::forward<BoundArgs>(bound))...);
  using BoundTuple = decltype(bound_tuple);

  internal::BindState<CallbackSig, std::remove_cvref_t<Functor>, BoundTuple>
      state(std::forward<Functor>(functor), std::move(bound_tuple));

  return OnceCallback<CallbackSig>(std::move(state));
}

// ---------------------------------------------------------------------------
// BindRepeating
// ---------------------------------------------------------------------------
template <typename Functor, typename... BoundArgs>
auto BindRepeating(Functor&& functor, BoundArgs&&... bound) {
  using Traits = internal::CallableTraits<std::remove_cvref_t<Functor>>;
  using RunType = typename Traits::RunType;
  using CallbackSig =
      typename internal::DropFirstN<RunType, sizeof...(BoundArgs)>::type;

  auto bound_tuple = std::make_tuple(
      static_cast<typename internal::StorageType<BoundArgs>::type>(
          std::forward<BoundArgs>(bound))...);
  using BoundTuple = decltype(bound_tuple);

  auto shared = std::make_shared<internal::BindState<
      CallbackSig, std::remove_cvref_t<Functor>, BoundTuple>>(
      std::forward<Functor>(functor), std::move(bound_tuple));

  return RepeatingCallback<CallbackSig>(
      [shared](auto&&... rest) ->
      typename internal::ReturnOf<CallbackSig>::type {
        return (*shared)(decltype(rest)(rest)...);
      });
}

// DoNothing() and DoNothingRepeating() live in
// base/functional/callback_helpers.h, matching Chromium's split.

}  // namespace avbase::base

#endif  // AVBASE_BASE_FUNCTIONAL_BIND_H_
