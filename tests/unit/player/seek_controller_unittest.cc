// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/seek_controller.h"

#include <gtest/gtest.h>

#include "base/time/time.h"

namespace avbase::player {
namespace {

using Outcome = SeekController::Outcome;

TEST(SeekControllerTest, ReachedWinsEvenAtTheDeadline) {
  const base::TimeTicks now = base::TimeTicks();
  // A frame landing exactly when the timeout fires is a success, not a race:
  // the user asked to land there, and the display did.
  EXPECT_EQ(Outcome::kReached, SeekController::Evaluate(true, now, now));
}

TEST(SeekControllerTest, PendingBeforeTheDeadline) {
  const base::TimeTicks now = base::TimeTicks();
  EXPECT_EQ(Outcome::kPending,
            SeekController::Evaluate(false, now + base::Seconds(5), now));
}

TEST(SeekControllerTest, ExpiredAtAndPastTheDeadline) {
  const base::TimeTicks deadline = base::TimeTicks();
  EXPECT_EQ(Outcome::kExpired,
            SeekController::Evaluate(false, deadline, deadline));
  EXPECT_EQ(Outcome::kExpired,
            SeekController::Evaluate(false, deadline,
                                     deadline + base::Milliseconds(1)));
}

TEST(SeekControllerTest, BeginTracksTheWaitUntilEnd) {
  SeekController c;
  EXPECT_FALSE(c.active());
  EXPECT_EQ(-1, c.End());

  c.Begin(42, base::Seconds(5), base::Seconds(2), base::TimeTicks());
  EXPECT_TRUE(c.active());
  EXPECT_EQ(42, c.request_id());
  EXPECT_EQ(base::Seconds(5), c.target());
  EXPECT_EQ(base::TimeTicks() + base::Seconds(2), c.deadline());

  EXPECT_EQ(42, c.End());
  EXPECT_FALSE(c.active());
  EXPECT_EQ(-1, c.End());
}

TEST(SeekControllerTest, ReplacingAWaitReplacesItsIdentity) {
  SeekController c;
  c.Begin(1, base::Seconds(3), base::Seconds(2), base::TimeTicks());
  // A second SeekTo supersedes: the caller completes the old request and
  // Begin overwrites every field, so a stale id can never be answered.
  c.Begin(2, base::Seconds(7), base::Seconds(2),
          base::TimeTicks() + base::Seconds(1));
  EXPECT_EQ(2, c.request_id());
  EXPECT_EQ(base::Seconds(7), c.target());
  EXPECT_EQ(base::TimeTicks() + base::Seconds(3), c.deadline());
  EXPECT_EQ(2, c.End());
}

}  // namespace
}  // namespace avbase::player
