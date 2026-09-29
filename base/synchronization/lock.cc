// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/synchronization/lock.h"

#include <algorithm>

#if defined(IJKPP_ENABLE_DCHECK)
#include <vector>

#include "base/logging.h"
#endif

namespace ijkpp::base {

#if defined(IJKPP_ENABLE_DCHECK)
namespace {

// Per-thread stack of currently held locks. A lock acquired while another is
// held must have been acquired *after* it in at least one observed ordering;
// if the reverse ordering was also observed, the two can deadlock.
//
// Chromium heap-allocates these and leaks them deliberately, to avoid touching
// a thread_local after its destructor ran during AtExit. ijkpp does not take
// locks from exit handlers, so a plain thread_local is used instead: it is
// destroyed at thread exit and LeakSanitizer stays clean, which matters
// because "0 leaks under LSan" is a release blocker (docs/07 §9.1).
std::vector<const Lock*>& HeldStack() {
  static thread_local std::vector<const Lock*> stack;
  return stack;
}

std::vector<std::pair<const void*, const void*>>& ObservedOrder() {
  static std::vector<std::pair<const void*, const void*>> orders;
  return orders;
}

}  // namespace

void Lock::AssertAcquiredInOrder() {
  auto& held = HeldStack();
  for (const Lock* outer : held) {
    const auto forward = std::make_pair(static_cast<const void*>(outer),
                                        static_cast<const void*>(this));
    const auto reverse = std::make_pair(static_cast<const void*>(this),
                                        static_cast<const void*>(outer));
    auto& orders = ObservedOrder();
    if (std::find(orders.begin(), orders.end(), reverse) != orders.end() &&
        std::find(orders.begin(), orders.end(), forward) == orders.end()) {
      LOG(FATAL) << "Lock order inversion detected: this Lock was acquired "
                    "both before and after another Lock. See docs/04 §3.1 for "
                    "the single global ordering all ijkpp locks must follow.";
    }
    if (std::find(orders.begin(), orders.end(), forward) == orders.end()) {
      orders.push_back(forward);
    }
  }
  held.push_back(this);
}

void Lock::RecordRelease() {
  auto& held = HeldStack();
  held.erase(std::remove(held.begin(), held.end(), this), held.end());
}

#else   // IJKPP_ENABLE_DCHECK

void Lock::AssertAcquiredInOrder() {}
void Lock::RecordRelease() {}

#endif  // IJKPP_ENABLE_DCHECK

}  // namespace ijkpp::base
