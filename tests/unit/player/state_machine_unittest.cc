// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/state_machine.h"

#include <gtest/gtest.h>

namespace ijkpp {
namespace {

TEST(PlayerStateMachineTest, StartsIdleAndWalksTheHappyPath) {
  PlayerStateMachine m;
  EXPECT_EQ(m.state(), PlayerState::kIdle);
  EXPECT_TRUE(m.TransitionTo(PlayerState::kInitialized));
  EXPECT_TRUE(m.TransitionTo(PlayerState::kPreparing));
  EXPECT_TRUE(m.TransitionTo(PlayerState::kPrepared));
  EXPECT_TRUE(m.TransitionTo(PlayerState::kStarted));
  EXPECT_TRUE(m.TransitionTo(PlayerState::kCompleted));
}

TEST(PlayerStateMachineTest, PauseAndResume) {
  PlayerStateMachine m;
  m.TransitionTo(PlayerState::kInitialized);
  m.TransitionTo(PlayerState::kPreparing);
  m.TransitionTo(PlayerState::kPrepared);
  m.TransitionTo(PlayerState::kStarted);
  EXPECT_TRUE(m.TransitionTo(PlayerState::kPaused));
  EXPECT_TRUE(m.TransitionTo(PlayerState::kStarted));
}

TEST(PlayerStateMachineTest, IllegalTransitionsAreRejected) {
  PlayerStateMachine m;
  // Cannot prepare without a source.
  EXPECT_FALSE(m.TransitionTo(PlayerState::kPreparing));
  // Cannot start from kIdle.
  EXPECT_FALSE(m.TransitionTo(PlayerState::kStarted));
  // SetDataSource twice is rejected (state stays kIdle after the failures).
  EXPECT_EQ(m.state(), PlayerState::kIdle);
  EXPECT_TRUE(m.TransitionTo(PlayerState::kInitialized));
  EXPECT_FALSE(m.TransitionTo(PlayerState::kInitialized));
  EXPECT_EQ(m.state(), PlayerState::kInitialized);
}

TEST(PlayerStateMachineTest, StopWalksThroughStoppingThenResets) {
  PlayerStateMachine m;
  m.TransitionTo(PlayerState::kInitialized);
  m.TransitionTo(PlayerState::kPreparing);
  m.TransitionTo(PlayerState::kPrepared);
  m.TransitionTo(PlayerState::kStarted);
  EXPECT_TRUE(m.TransitionTo(PlayerState::kStopping));
  EXPECT_TRUE(m.TransitionTo(PlayerState::kStopped));
  EXPECT_TRUE(m.TransitionTo(PlayerState::kIdle));
  // And the player is reusable after Reset.
  EXPECT_TRUE(m.TransitionTo(PlayerState::kInitialized));
}

TEST(PlayerStateMachineTest, ErrorIsRecoverableOnlyThroughReset) {
  PlayerStateMachine m;
  m.TransitionTo(PlayerState::kInitialized);
  m.TransitionTo(PlayerState::kPreparing);
  EXPECT_TRUE(m.TransitionTo(PlayerState::kError));
  EXPECT_FALSE(m.TransitionTo(PlayerState::kStarted));
  // Stop is the sanctioned way out of kError; direct restart is not.
  EXPECT_TRUE(m.TransitionTo(PlayerState::kStopped));
  m.ForceReset();
  EXPECT_EQ(m.state(), PlayerState::kIdle);
  EXPECT_TRUE(m.TransitionTo(PlayerState::kInitialized));
}

}  // namespace
}  // namespace ijkpp
