// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/synchronization/lock.h"

#include <algorithm>

#if defined(AVBASE_ENABLE_DCHECK)
#include <vector>

#include "base/logging.h"
#endif

namespace avbase::base {

#if defined(AVBASE_ENABLE_DCHECK)
namespace {

// Per-thread stack of currently held locks. A lock acquired while another is
// held must have been acquired *after* it in at least one observed ordering;
// if the reverse ordering was also observed, the two can deadlock.
//
// Chromium heap-allocates these and leaks them deliberately, to avoid touching
// a thread_local after its destructor ran during AtExit. avbase does not take
// locks from exit handlers, so a plain thread_local is used instead: it is
// destroyed at thread exit and LeakSanitizer stays clean, which matters
// because "0 leaks under LSan" is a release blocker (docs/07 §9.1).
std::vector<const Lock*>& HeldStack() {
  static thread_local std::vector<const Lock*> stack;
  return stack;
}

// Per-thread for the same reason as the held stack above -- and this one was a
// real bug, not a formality: a plain function-local static is shared by every
// thread that takes a lock, and AssertAcquiredInOrder() records an ordering
// *before* the lock itself is acquired. Two threads pushing into one
// std::vector is a data race; in a RelWithDebInfo build it corrupted the heap
// and aborted rather than misbehaving politely ("libmalloc: pointer being
// freed was not allocated", about one playback run in thirty under load), and
// in a TSan build it is LockTest.ConcurrentOrderRecordingIsRaceFree that
// reports it.
//
// The cost of per-thread state: an ordering is only remembered by the thread
// that observed it, so a pattern where every thread takes its pair in its own
// fixed (and mutually opposite) order goes undetected. That is the price of
// not putting a global lock inside the lock-order checker.
std::vector<std::pair<const void*, const void*>>& ObservedOrder() {
  static thread_local std::vector<std::pair<const void*, const void*>> orders;
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
                    "the single global ordering all avbase locks must follow.";
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

#else  // AVBASE_ENABLE_DCHECK

void Lock::AssertAcquiredInOrder() {}
void Lock::RecordRelease() {}

#endif  // AVBASE_ENABLE_DCHECK

}  // namespace avbase::base
