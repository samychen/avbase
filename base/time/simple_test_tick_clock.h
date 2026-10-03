// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/test/simple_test_tick_clock.h` (BSD-3-Clause).

#ifndef AVBASE_BASE_TIME_SIMPLE_TEST_TICK_CLOCK_H_
#define AVBASE_BASE_TIME_SIMPLE_TEST_TICK_CLOCK_H_

#include <atomic>

#include "base/time/tick_clock.h"

namespace avbase::base {

// A TickClock whose time only moves when the test moves it. Every scheduling
// test in media/filters/ uses this, which is why avbase can exhaustively test
// logic that ijkplayer cannot test at all (it reads av_gettime() directly).
class SimpleTestTickClock final : public TickClock {
 public:
  SimpleTestTickClock() = default;
  SimpleTestTickClock(const SimpleTestTickClock&) = delete;
  SimpleTestTickClock& operator=(const SimpleTestTickClock&) = delete;
  ~SimpleTestTickClock() override = default;

  TimeTicks NowTicks() const override {
    return TimeTicks::FromMicroseconds(
        now_micros_.load(std::memory_order_relaxed));
  }

  void Advance(TimeDelta delta) {
    now_micros_.fetch_add(delta.InMicroseconds(), std::memory_order_relaxed);
  }

  // Jumps to an absolute point in time. |now| must not be before the current
  // time; tests that need to rewind should construct a new clock.
  void StartAt(TimeTicks now) {
    now_micros_.store(now.since_origin_micros(), std::memory_order_relaxed);
  }

 private:
  std::atomic<int64_t> now_micros_{0};
};

}  // namespace avbase::base

#endif  // AVBASE_BASE_TIME_SIMPLE_TEST_TICK_CLOCK_H_
