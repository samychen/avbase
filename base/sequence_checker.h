// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/sequence_checker.h` (BSD-3-Clause).

#ifndef IJKPP_BASE_SEQUENCE_CHECKER_H_
#define IJKPP_BASE_SEQUENCE_CHECKER_H_

#include <atomic>
#include <thread>

#include "base/check.h"

namespace ijkpp::base {

// Records the thread id on first use and DCHECKs every later use matches.
// ijkpp runs one logical sequence per dedicated thread (docs/04 §2), so
// thread identity is a faithful stand-in for Chromium's sequence tokens.
class SequenceChecker {
 public:
  SequenceChecker() = default;
  SequenceChecker(const SequenceChecker&) = delete;
  SequenceChecker& operator=(const SequenceChecker&) = delete;
  ~SequenceChecker() = default;

  bool CalledOnValidSequence() const {
    const std::thread::id id = std::this_thread::get_id();
    std::thread::id expected{};
    if (bound_.compare_exchange_strong(expected, id, std::memory_order_acq_rel)) {
      return true;   // First call binds.
    }
    return expected == id;
  }

  void DetachFromSequence() {
    bound_.store(std::thread::id{}, std::memory_order_release);
  }

 private:
  mutable std::atomic<std::thread::id> bound_{};
};

}  // namespace ijkpp::base

#if defined(IJKPP_ENABLE_DCHECK)
#define SEQUENCE_CHECKER(name) mutable ::ijkpp::base::SequenceChecker name
#define DCHECK_CALLED_ON_VALID_SEQUENCE(name)               \
  DCHECK((name).CalledOnValidSequence())
#define DETACH_FROM_SEQUENCE(name) (name).DetachFromSequence()
#else
#define SEQUENCE_CHECKER(name)
#define DCHECK_CALLED_ON_VALID_SEQUENCE(name) static_assert(true, "")
#define DETACH_FROM_SEQUENCE(name) static_assert(true, "")
#endif

#endif  // IJKPP_BASE_SEQUENCE_CHECKER_H_
