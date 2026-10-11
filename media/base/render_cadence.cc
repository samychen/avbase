// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/render_cadence.h"

namespace avbase::media {

FramePacing NextFrameDelay(base::TimeTicks frame_start, base::TimeTicks now,
                           base::TimeTicks previous_deadline,
                           base::TimeDelta interval) {
  // A non-positive interval is not a cadence. Answering "go now, unanchored"
  // keeps a caller that computed one from aiming at a deadline it can never
  // reach, which would otherwise look like a render loop pinned at 100%.
  if (interval <= base::TimeDelta()) {
    return {base::TimeDelta(), base::TimeTicks()};
  }

  const base::TimeDelta spent = now - frame_start;
  // Half a refresh is the threshold: a present that took at least that long
  // was waiting for something, not drawing. It already did the waiting, so
  // send the loop straight back -- and DROP the anchor, because from here the
  // refresh decides the cadence and a stale deadline would only fight it.
  if (spent > interval / 2) {
    return {base::TimeDelta(), base::TimeTicks()};
  }

  base::TimeTicks deadline = previous_deadline.is_null()
                                 ? frame_start + interval
                                 : previous_deadline + interval;
  // Fell behind by more than a period (a seek, a window drag, a GC). Do NOT
  // catch up: N frames back to back would put the picture in a hurry and
  // deliver none of them on time.
  if (now >= deadline) {
    deadline = now;
  }
  return {deadline - now, deadline};
}

}  // namespace avbase::media
