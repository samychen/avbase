// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/seek_controller.h"

namespace avbase::player {

// static
SeekController::Outcome SeekController::Evaluate(bool target_reached,
                                                 base::TimeTicks deadline,
                                                 base::TimeTicks now) {
  // Reached wins over expired: a frame that lands on the tick the timeout
  // fires is a success, not a race. Everything else pending past the
  // deadline is the Δ10 timeout.
  if (target_reached) {
    return Outcome::kReached;
  }
  if (now >= deadline) {
    return Outcome::kExpired;
  }
  return Outcome::kPending;
}

void SeekController::Begin(int64_t request_id, base::TimeDelta target,
                           base::TimeDelta timeout, base::TimeTicks now) {
  active_ = true;
  request_id_ = request_id;
  target_ = target;
  deadline_ = now + timeout;
}

int64_t SeekController::End() {
  const int64_t id = active_ ? request_id_ : -1;
  active_ = false;
  request_id_ = -1;
  target_ = base::TimeDelta();
  deadline_ = base::TimeTicks();
  return id;
}

}  // namespace avbase::player
