// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_PLAYER_STATE_MACHINE_H_
#define IJKPP_PLAYER_STATE_MACHINE_H_

#include "player/public/player_event.h"
#include "player/public/player_export.h"

namespace ijkpp {

// The SDK's playback states and the only transitions between them.
//
// WHY A TABLE AND NOT IF-CHAINS AT THE CALL SITES: ijkplayer's mp->mp_state
// is a bag of state bits mutated from six threads, and "can this happen now"
// is answered differently in a dozen places (docs/01 defect #3). Here every
// caller asks one function, and an illegal request is rejected with
// ErrorCode::kInvalidState naming the states involved -- the actionable-error
// rule of docs/10 §4 applied to control flow.
//
// Transitions (docs/03 §9):
//
//   kIdle --SetDataSource--> kInitialized --Prepare--> kPreparing
//     --ready--> kPrepared --Start--> kStarted <--Pause/Pause--> kPaused
//   kStarted --EOS--> kCompleted --Start--> kStarted (replay)
//   any of {kPrepared, kStarted, kPaused, kCompleted} --Stop--> kStopping
//     --stopped--> kStopped --Reset--> kIdle
//   any --error--> kError --Reset--> kIdle
class IJKPP_PLAYER_EXPORT PlayerStateMachine {
 public:
  PlayerStateMachine() = default;
  PlayerStateMachine(const PlayerStateMachine&) = delete;
  PlayerStateMachine& operator=(const PlayerStateMachine&) = delete;

  PlayerState state() const { return state_; }
  // Returns false and leaves |state_| unchanged when the transition is
  // illegal. Legal or not, the caller owns publishing kStateChanged.
  bool TransitionTo(PlayerState to);
  // Back to kIdle unconditionally: Reset() is the documented way out of any
  // state, including kError.
  void ForceReset();

 private:
  bool CanTransitionTo(PlayerState to) const;
  PlayerState state_{PlayerState::kIdle};
};

}  // namespace ijkpp

#endif  // IJKPP_PLAYER_STATE_MACHINE_H_
