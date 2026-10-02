// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/live_edge_policy.h"

namespace avbase::media {

// static
bool LiveEdgePolicy::ShouldChase(bool is_live, base::TimeDelta latency_hint,
                                 base::TimeDelta behind) {
  if (!is_live || latency_hint <= base::TimeDelta()) {
    return false;
  }
  return behind > latency_hint;
}

// static
base::TimeDelta LiveEdgePolicy::SuggestedTarget(base::TimeDelta live_edge,
                                                base::TimeDelta latency_hint) {
  // Half the target of headroom: landing exactly at the edge would trip the
  // chase again on the next statistics tick as soon as the consumer falls
  // behind by anything; landing half-way gives it the full target of slack.
  return live_edge - latency_hint / 2;
}

}  // namespace avbase::media
