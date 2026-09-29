// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_BASE_TIME_DEFAULT_TICK_CLOCK_H_
#define IJKPP_BASE_TIME_DEFAULT_TICK_CLOCK_H_

#include "base/base_export.h"
#include "base/time/tick_clock.h"

namespace ijkpp::base {

class IJKPP_BASE_EXPORT DefaultTickClock final : public TickClock {
 public:
  DefaultTickClock() = default;
  DefaultTickClock(const DefaultTickClock&) = delete;
  DefaultTickClock& operator=(const DefaultTickClock&) = delete;
  ~DefaultTickClock() override = default;

  TimeTicks NowTicks() const override { return TimeTicks::Now(); }

  // Shared instance; safe to hand out as a raw pointer because it never dies.
  static const DefaultTickClock* GetInstance();
};

}  // namespace ijkpp::base

#endif  // IJKPP_BASE_TIME_DEFAULT_TICK_CLOCK_H_
