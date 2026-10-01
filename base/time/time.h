// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/time/time.h` (BSD-3-Clause).
//
// Three distinct types, never interchangeable:
//   TimeDelta  a duration          (media durations, timeouts)
//   TimeTicks  monotonic wall time (scheduling, deadlines)
//   Time       calendar wall time  (wall-clock timestamps)
//
// This replaces ijkplayer's habit of passing raw int64_t values whose unit
// (microseconds / milliseconds / stream ticks / seconds-as-double) is only
// discoverable by reading the callee. See docs/01 §2 病灶 5.

#ifndef AVBASE_BASE_TIME_TIME_H_
#define AVBASE_BASE_TIME_TIME_H_

#include <stdint.h>

#include <chrono>
#include <compare>
#include <limits>
#include <ostream>

#include "base/base_export.h"

namespace avbase::base {

// ---------------------------------------------------------------------------
// TimeDelta
// ---------------------------------------------------------------------------
class AVBASE_BASE_EXPORT TimeDelta {
 public:
  constexpr TimeDelta() noexcept = default;

  // Copyable, assignable, trivially so.
  constexpr TimeDelta(const TimeDelta&) noexcept = default;
  constexpr TimeDelta& operator=(const TimeDelta&) noexcept = default;

  constexpr bool is_zero() const noexcept { return micros_ == 0; }
  constexpr bool is_infinte() const noexcept {
    return micros_ == std::numeric_limits<int64_t>::max() ||
           micros_ == std::numeric_limits<int64_t>::min();
  }
  constexpr bool is_max() const noexcept {
    return micros_ == std::numeric_limits<int64_t>::max();
  }
  constexpr bool is_min() const noexcept {
    return micros_ == std::numeric_limits<int64_t>::min();
  }
  constexpr bool is_negative() const noexcept { return micros_ < 0; }

  constexpr int64_t InMicroseconds() const noexcept { return micros_; }
  constexpr int64_t InMilliseconds() const noexcept { return micros_ / 1000; }
  constexpr int64_t InSeconds() const noexcept { return micros_ / 1000000; }
  constexpr int64_t InMinutes() const noexcept { return micros_ / 60000000; }
  constexpr double InMillisecondsF() const noexcept {
    return static_cast<double>(micros_) / 1000.0;
  }
  constexpr double InSecondsF() const noexcept {
    return static_cast<double>(micros_) / 1000000.0;
  }

  constexpr TimeDelta& operator+=(TimeDelta other) noexcept {
    micros_ = SaturatingAdd(micros_, other.micros_);
    return *this;
  }
  constexpr TimeDelta& operator-=(TimeDelta other) noexcept {
    micros_ = SaturatingAdd(micros_, -other.micros_);
    return *this;
  }
  constexpr TimeDelta operator-() const noexcept {
    return FromMicroseconds(micros_ == std::numeric_limits<int64_t>::min()
                                ? std::numeric_limits<int64_t>::max()
                                : -micros_);
  }

  // Scaled arithmetic. Multiplication by a scalar saturates.
  constexpr TimeDelta operator*(int64_t a) const noexcept {
    if (is_infinte() || a == std::numeric_limits<int64_t>::max()) {
      return (micros_ < 0) == (a < 0) ? Max() : Min();
    }
    const int64_t result = micros_ * a;
    if (a != 0 && result / a != micros_) {
      return (micros_ < 0) == (a < 0) ? Max() : Min();
    }
    return FromMicroseconds(result);
  }
  constexpr TimeDelta operator/(int64_t a) const noexcept {
    if (a == 0) {
      return (micros_ < 0) ? Min() : Max();
    }
    return is_infinte() ? *this : FromMicroseconds(micros_ / a);
  }
  constexpr double operator/(TimeDelta a) const noexcept {
    return a.is_zero() ? std::numeric_limits<double>::infinity()
                       : static_cast<double>(micros_) /
                             static_cast<double>(a.micros_);
  }
  constexpr TimeDelta operator%(TimeDelta a) const noexcept {
    return FromMicroseconds(a.is_zero() ? micros_ : micros_ % a.micros_);
  }

  constexpr friend TimeDelta operator+(TimeDelta a, TimeDelta b) noexcept {
    return a += b;
  }
  constexpr friend TimeDelta operator-(TimeDelta a, TimeDelta b) noexcept {
    return a -= b;
  }
  constexpr friend TimeDelta operator*(int64_t a, TimeDelta d) noexcept {
    return d * a;
  }
  constexpr friend auto operator<=>(TimeDelta, TimeDelta) noexcept = default;
  constexpr friend bool operator==(TimeDelta, TimeDelta) noexcept = default;

  // Factories. Named rather than a bare constructor so the unit is always
  // visible at the call site: `base::Milliseconds(40)`, never `40`.
  static constexpr TimeDelta Zero() noexcept { return TimeDelta(); }
  static constexpr TimeDelta Min() noexcept {
    return FromMicroseconds(std::numeric_limits<int64_t>::min());
  }
  static constexpr TimeDelta Max() noexcept {
    return FromMicroseconds(std::numeric_limits<int64_t>::max());
  }
  static constexpr TimeDelta FromMicroseconds(int64_t us) noexcept {
    return TimeDelta(us);
  }
  static constexpr TimeDelta FromMilliseconds(int64_t ms) noexcept {
    return TimeDelta(SaturatingMul(ms, 1000));
  }
  static constexpr TimeDelta FromSeconds(int64_t s) noexcept {
    return TimeDelta(SaturatingMul(s, 1000000));
  }
  static constexpr TimeDelta FromSecondsD(double s) noexcept {
    return TimeDelta(static_cast<int64_t>(s * 1000000.0));
  }
  static constexpr TimeDelta FromMinutes(int64_t m) noexcept {
    return TimeDelta(SaturatingMul(m, 60000000));
  }

  constexpr std::chrono::microseconds ToChronoMicros() const noexcept {
    return std::chrono::microseconds(micros_);
  }

  std::string ToString() const;

 private:
  constexpr explicit TimeDelta(int64_t micros) noexcept : micros_(micros) {}

  static constexpr int64_t SaturatingAdd(int64_t a, int64_t b) noexcept {
    const int64_t kMax = std::numeric_limits<int64_t>::max();
    const int64_t kMin = std::numeric_limits<int64_t>::min();
    if (b > 0 && a > kMax - b) return kMax;
    if (b < 0 && a < kMin - b) return kMin;
    return a + b;
  }
  static constexpr int64_t SaturatingMul(int64_t a, int64_t b) noexcept {
    if (a == 0) return 0;
    const int64_t r = a * b;
    if (r / a != b) return (a < 0) == (b < 0) ? std::numeric_limits<int64_t>::max()
                                              : std::numeric_limits<int64_t>::min();
    return r;
  }

  int64_t micros_{0};
};

constexpr TimeDelta Microseconds(int64_t us) noexcept {
  return TimeDelta::FromMicroseconds(us);
}
constexpr TimeDelta Milliseconds(int64_t ms) noexcept {
  return TimeDelta::FromMilliseconds(ms);
}
constexpr TimeDelta Seconds(int64_t s) noexcept { return TimeDelta::FromSeconds(s); }
constexpr TimeDelta SecondsD(double s) noexcept { return TimeDelta::FromSecondsD(s); }
constexpr TimeDelta Minutes(int64_t m) noexcept { return TimeDelta::FromMinutes(m); }

AVBASE_BASE_EXPORT std::ostream& operator<<(std::ostream& os, TimeDelta d);

// ---------------------------------------------------------------------------
// TimeTicks — monotonic. Use for scheduling, deadlines and elapsed time.
// ---------------------------------------------------------------------------
class AVBASE_BASE_EXPORT TimeTicks {
 public:
  constexpr TimeTicks() noexcept = default;

  // Default-constructed TimeTicks is null; this distinguishes "no time yet"
  // from "the epoch", which matters for frame_timer initialization.
  constexpr bool is_null() const noexcept { return micros_ == 0; }

  static TimeTicks Now();
  static constexpr TimeTicks FromMicroseconds(int64_t us) noexcept {
    return TimeTicks(us);
  }
  constexpr int64_t since_origin_micros() const noexcept { return micros_; }

  constexpr TimeTicks operator+(TimeDelta d) const noexcept {
    return TimeTicks(micros_ + d.InMicroseconds());
  }
  constexpr TimeTicks operator-(TimeDelta d) const noexcept {
    return TimeTicks(micros_ - d.InMicroseconds());
  }
  constexpr TimeDelta operator-(TimeTicks other) const noexcept {
    return TimeDelta::FromMicroseconds(micros_ - other.micros_);
  }
  constexpr TimeTicks& operator+=(TimeDelta d) noexcept {
    micros_ += d.InMicroseconds();
    return *this;
  }
  constexpr TimeTicks& operator-=(TimeDelta d) noexcept {
    micros_ -= d.InMicroseconds();
    return *this;
  }
  constexpr friend auto operator<=>(TimeTicks, TimeTicks) noexcept = default;
  constexpr friend bool operator==(TimeTicks, TimeTicks) noexcept = default;

  std::string ToString() const;

 private:
  constexpr explicit TimeTicks(int64_t micros) noexcept : micros_(micros) {}
  int64_t micros_{0};
};

AVBASE_BASE_EXPORT std::ostream& operator<<(std::ostream& os, TimeTicks t);

// ---------------------------------------------------------------------------
// Time — calendar wall clock (microseconds since Windows epoch, as in
// Chromium, so that serialized values are comparable across the two).
// ---------------------------------------------------------------------------
class AVBASE_BASE_EXPORT Time {
 public:
  constexpr Time() noexcept = default;
  static Time Now();
  static constexpr Time FromUnixTime(int64_t seconds) noexcept {
    return Time(seconds * 1000000 + kUnixEpochDeltaMicros);
  }
  static constexpr Time FromUnixMillis(int64_t millis) noexcept {
    return Time(millis * 1000 + kUnixEpochDeltaMicros);
  }
  constexpr bool is_null() const noexcept { return micros_ == 0; }
  constexpr int64_t ToUnixMillis() const noexcept {
    return (micros_ - kUnixEpochDeltaMicros) / 1000;
  }
  constexpr Time operator+(TimeDelta d) const noexcept {
    return Time(micros_ + d.InMicroseconds());
  }
  constexpr Time operator-(TimeDelta d) const noexcept {
    return Time(micros_ - d.InMicroseconds());
  }
  constexpr TimeDelta operator-(Time other) const noexcept {
    return TimeDelta::FromMicroseconds(micros_ - other.micros_);
  }
  constexpr friend auto operator<=>(Time, Time) noexcept = default;
  constexpr friend bool operator==(Time, Time) noexcept = default;

 private:
  // 1601-01-01 -> 1970-01-01, in microseconds.
  static constexpr int64_t kUnixEpochDeltaMicros = 11644473600000000LL;
  constexpr explicit Time(int64_t micros) noexcept : micros_(micros) {}
  int64_t micros_{0};
};

}  // namespace avbase::base

#endif  // AVBASE_BASE_TIME_TIME_H_
