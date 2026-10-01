// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_BASE_SYNCHRONIZATION_ATOMIC_FLAG_H_
#define AVBASE_BASE_SYNCHRONIZATION_ATOMIC_FLAG_H_

#include <atomic>

namespace avbase::base {

// A settable boolean flag for cross-thread cancellation.
//
// Chromium's AtomicFlag is deliberately one-way (Set only). avbase adds Reset()
// because the demuxer's interrupt flag must be cleared before each seek: it is
// set to break a blocking av_read_frame, then cleared so the next read can
// proceed. Stop flags stay one-way in practice — nothing ever resets them.
//
// avbase uses exactly three of these per player, replacing ijkplayer's
// `abort_request` plus five separate SDL_CondSignal call sites (docs/04 §2.1).
// Missing one of those signals is what made ijkplayer's release() hang.
class AtomicFlag {
 public:
  AtomicFlag() = default;
  AtomicFlag(const AtomicFlag&) = delete;
  AtomicFlag& operator=(const AtomicFlag&) = delete;
  ~AtomicFlag() = default;

  void Set() { flag_.store(true, std::memory_order_release); }
  bool IsSet() const { return flag_.load(std::memory_order_acquire); }
  void Reset() { flag_.store(false, std::memory_order_release); }

 private:
  std::atomic<bool> flag_{false};
};

}  // namespace avbase::base

#endif  // AVBASE_BASE_SYNCHRONIZATION_ATOMIC_FLAG_H_
