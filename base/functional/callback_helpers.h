// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/functional/callback_helpers.h` (BSD-3-Clause).

#ifndef AVBASE_BASE_FUNCTIONAL_CALLBACK_HELPERS_H_
#define AVBASE_BASE_FUNCTIONAL_CALLBACK_HELPERS_H_

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"

namespace avbase::base {

// A closure that does nothing. Prefer this over binding an empty lambda so
// that intent is explicit at the call site.
// Returns a *runnable* empty closure rather than a null one, matching
// Chromium: `std::move(cb).Run()` on a "no completion action" callback is then
// always safe and never trips the null-callback CHECK.
inline OnceClosure DoNothing() {
  return OnceCallback<void()>([]() {});
}

inline RepeatingClosure DoNothingRepeating() {
  return RepeatingCallback<void()>([]() {});
}

// Wraps a OnceClosure and runs it if it has not been run by the time this
// object is destroyed. Useful for "always signal completion" guarantees.
class ScopedClosureRunner {
 public:
  ScopedClosureRunner() = default;
  explicit ScopedClosureRunner(OnceClosure closure)
      : closure_(std::move(closure)) {}
  ScopedClosureRunner(const ScopedClosureRunner&) = delete;
  ScopedClosureRunner& operator=(const ScopedClosureRunner&) = delete;
  ScopedClosureRunner(ScopedClosureRunner&& other) noexcept
      : closure_(std::move(other.closure_)) {}
  ScopedClosureRunner& operator=(ScopedClosureRunner&& other) noexcept {
    if (this != &other) {
      Run();
      closure_ = std::move(other.closure_);
    }
    return *this;
  }
  ~ScopedClosureRunner() { Run(); }

  void Run() {
    if (closure_) {
      std::move(closure_).Run();
    }
  }
  void Reset() { closure_.Reset(); }
  void Reset(OnceClosure closure) {
    Run();
    closure_ = std::move(closure);
  }
  OnceClosure Release() { return std::move(closure_); }

 private:
  OnceClosure closure_;
};

}  // namespace avbase::base

#endif  // AVBASE_BASE_FUNCTIONAL_CALLBACK_HELPERS_H_
