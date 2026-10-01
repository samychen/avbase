// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/time/tick_clock.h` (BSD-3-Clause).

#ifndef AVBASE_BASE_TIME_TICK_CLOCK_H_
#define AVBASE_BASE_TIME_TICK_CLOCK_H_

#include "base/base_export.h"
#include "base/time/time.h"

namespace avbase::base {

// Injectable monotonic clock. Production code uses DefaultTickClock; tests use
// SimpleTestTickClock so that scheduling logic becomes deterministic and no
// test ever sleeps. This is design principle P6 (docs/01 §4).
class AVBASE_BASE_EXPORT TickClock {
 public:
  virtual ~TickClock() = default;
  virtual TimeTicks NowTicks() const = 0;
};

}  // namespace avbase::base

#endif  // AVBASE_BASE_TIME_TICK_CLOCK_H_
