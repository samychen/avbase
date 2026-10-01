// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_BASE_SYNCHRONIZATION_ATOMIC_SEQUENCE_NUMBER_H_
#define AVBASE_BASE_SYNCHRONIZATION_ATOMIC_SEQUENCE_NUMBER_H_

#include <atomic>

namespace avbase::base {

// Atomic counter used for seek generation numbers (`serial`) and request ids.
class AtomicSequenceNumber {
 public:
  AtomicSequenceNumber() = default;
  AtomicSequenceNumber(const AtomicSequenceNumber&) = delete;
  AtomicSequenceNumber& operator=(const AtomicSequenceNumber&) = delete;
  ~AtomicSequenceNumber() = default;

  // Increments by |n| and returns the NEW value.
  int Add(int n) { return seq_.fetch_add(n, std::memory_order_acq_rel) + n; }
  int GetNext() { return Add(1); }
  int Get() const { return seq_.load(std::memory_order_acquire); }

 private:
  std::atomic<int> seq_{0};
};

}  // namespace avbase::base

#endif  // AVBASE_BASE_SYNCHRONIZATION_ATOMIC_SEQUENCE_NUMBER_H_
