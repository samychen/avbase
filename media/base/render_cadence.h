// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_BASE_RENDER_CADENCE_H_
#define AVBASE_MEDIA_BASE_RENDER_CADENCE_H_

#include "base/time/time.h"
#include "media/media_export.h"

namespace avbase::media {

// What a render loop should do next.
struct AVBASE_MEDIA_EXPORT FramePacing {
  // How long to sleep before the next frame. Zero means "go now".
  base::TimeDelta delay;

  // The deadline to hand back to the NEXT call to NextFrameDelay(). A null
  // TimeTicks means "unanchored": the timer is not the pacing source here, so
  // the next frame has to re-anchor from its own start.
  base::TimeTicks next_deadline;
};

// How long a render loop should wait after the frame it just presented.
//
// The whole difficulty is that TWO different things can be the throttle, and
// which one it is changes from frame to frame:
//
//   * The present blocked on the refresh (SwapBuffers, or SDL_RenderPresent
//     with vsync). Then the waiting HAS ALREADY HAPPENED and the timer must
//     not add another interval on top of it -- that double-count is what turns
//     a 60 fps source into 30.
//   * The present returned at once (no vsync: offscreen, or a driver with the
//     interval at 0). Then the timer is the ONLY pacing source and it has to
//     supply the whole interval.
//
// The second case is where "measure from now" quietly loses frames:
// PostDelayedTask never wakes exactly when asked (~1.8 ms late as measured on
// this machine), so budgeting the next interval from now charges that
// overshoot to EVERY period -- a 60 fps source settles at ~53 fps and no
// single frame looks wrong enough to notice. Anchoring on the PREVIOUSLY
// PLANNED deadline makes the errors cancel instead: a frame that woke late
// sleeps less next time.
//
// Every call is pure -- it takes `now` rather than reading the clock -- so the
// cadence can be tested without a display, a runner, or a clock.
AVBASE_MEDIA_EXPORT FramePacing
NextFrameDelay(base::TimeTicks frame_start, base::TimeTicks now,
               base::TimeTicks previous_deadline, base::TimeDelta interval);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_RENDER_CADENCE_H_
