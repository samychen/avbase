// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/time/default_tick_clock.h"

namespace avbase::base {

const DefaultTickClock* DefaultTickClock::GetInstance() {
  static const DefaultTickClock* instance = new DefaultTickClock();
  return instance;
}

}  // namespace avbase::base
