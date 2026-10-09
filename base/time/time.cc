// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/time/time.h"

#include <chrono>
#include <cmath>
#include <string>

namespace avbase::base {

namespace {

int64_t MonotonicNowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

TimeTicks TimeTicks::Now() {
  return FromMicroseconds(MonotonicNowMicros());
}

Time Time::Now() {
  const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  return FromUnixTime(0) + Microseconds(us);
}

std::string TimeDelta::ToString() const {
  if (is_infinte()) {
    return micros_ > 0 ? "inf" : "-inf";
  }
  const double ms = InMillisecondsF();
  if (std::fabs(ms) >= 1000.0) {
    return std::to_string(InSecondsF()) + "s";
  }
  return std::to_string(ms) + "ms";
}

std::string TimeTicks::ToString() const {
  // Explicit cast: micros_ is int64_t and -Wconversion (enabled by the debug
  // preset via -Werror) rejects the implicit widening to double, because an
  // int64 magnitude above 2^53 would lose precision. That is unreachable for
  // a wall-clock offset, but the cast documents it rather than silencing it.
  return std::to_string(static_cast<double>(micros_) / 1000.0) + "ms";
}

std::ostream& operator<<(std::ostream& os, TimeDelta d) {
  return os << d.InMicroseconds() << "us";
}

std::ostream& operator<<(std::ostream& os, TimeTicks t) {
  return os << t.since_origin_micros() << "us";
}

}  // namespace avbase::base
