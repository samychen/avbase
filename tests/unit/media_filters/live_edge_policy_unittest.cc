// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/live_edge_policy.h"

#include <gtest/gtest.h>

#include "base/time/time.h"

namespace avbase::media {
namespace {

using Policy = LiveEdgePolicy;

TEST(LiveEdgePolicyTest, NeverChasesWithoutALatencyTarget) {
  // config.net.live_max_latency == 0 means "do not chase": a VOD-style
  // consumer of a live stream opts out explicitly (docs/10 cookbook).
  EXPECT_FALSE(Policy::ShouldChase(true, base::TimeDelta(),
                                   base::Seconds(100)));
  EXPECT_FALSE(Policy::ShouldChase(true, base::Seconds(-1),
                                   base::Seconds(100)));
}

TEST(LiveEdgePolicyTest, NeverChasesNonLiveSources) {
  // A finite source falling behind is a decode-power problem, not a chase
  // situation -- chasing would eat content.
  EXPECT_FALSE(Policy::ShouldChase(false, base::Seconds(8),
                                   base::Seconds(100)));
}

TEST(LiveEdgePolicyTest, ChasesOnlyPastTheTarget) {
  const base::TimeDelta target = base::Seconds(8);
  EXPECT_FALSE(Policy::ShouldChase(true, target, base::Seconds(7)));
  EXPECT_FALSE(Policy::ShouldChase(true, target, base::Seconds(8)));
  EXPECT_TRUE(Policy::ShouldChase(true, target, base::Seconds(8) +
                                                 base::Milliseconds(1)));
  EXPECT_TRUE(Policy::ShouldChase(true, target, base::Seconds(30)));
}

TEST(LiveEdgePolicyTest, SuggestedTargetLandsHalfWayWithHeadroom) {
  // Landing exactly at the edge would trip the chase again on the next tick;
  // half the target of slack is what stops ping-pong.
  const base::TimeDelta edge = base::Seconds(100);
  const base::TimeDelta target = base::Seconds(8);
  EXPECT_EQ(Policy::SuggestedTarget(edge, target), base::Seconds(96));
  EXPECT_LT(Policy::SuggestedTarget(edge, target), edge);
  EXPECT_GE(Policy::SuggestedTarget(edge, target),
            edge - target);   // never further than the target itself
}

}  // namespace
}  // namespace avbase::media
