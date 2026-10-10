// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/state_machine.h"

namespace avbase {

bool PlayerStateMachine::TransitionTo(PlayerState to) {
  if (!CanTransitionTo(to)) {
    return false;
  }
  state_ = to;
  return true;
}

bool PlayerStateMachine::CanTransitionTo(PlayerState to) const {
  switch (state_) {
  case PlayerState::kIdle:
    // M3: stopping straight from kIdle is legal -- without this edge Stop()
    // could not move the machine at all (it stayed in kIdle while the event
    // broadcast a fictional kStopped -> kStopped transition).
    return to == PlayerState::kInitialized || to == PlayerState::kStopping;
  case PlayerState::kInitialized:
    return to == PlayerState::kPreparing || to == PlayerState::kStopping;
  case PlayerState::kPreparing:
    return to == PlayerState::kPrepared || to == PlayerState::kStopping ||
           to == PlayerState::kError;
  case PlayerState::kPrepared:
    return to == PlayerState::kStarted || to == PlayerState::kStopping ||
           to == PlayerState::kError;
  case PlayerState::kStarted:
    return to == PlayerState::kPaused || to == PlayerState::kCompleted ||
           to == PlayerState::kStopping || to == PlayerState::kError;
  case PlayerState::kPaused:
    return to == PlayerState::kStarted || to == PlayerState::kStopping ||
           to == PlayerState::kError;
  case PlayerState::kCompleted:
    return to == PlayerState::kStarted || to == PlayerState::kStopping ||
           to == PlayerState::kError;
  case PlayerState::kStopping:
    return to == PlayerState::kStopped;
  case PlayerState::kStopped:
    return to == PlayerState::kIdle;
  case PlayerState::kError:
    return to == PlayerState::kStopped;
  case PlayerState::kBuffering:
    // kBuffering is a projection of buffering events, not a state this
    // machine drives; kept in the enum for the frozen event surface.
    return false;
  case PlayerState::kEnd:
    return false;
  }
  return false;
}

void PlayerStateMachine::ForceReset() {
  state_ = PlayerState::kIdle;
}

}  // namespace avbase
