// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/buffer_controller.h"

#include <algorithm>

namespace avbase::player {

BufferController::BufferController() : BufferController(Thresholds{}) {}

BufferController::BufferController(Thresholds thresholds)
    : thresholds_(thresholds) {
  // The tier marks must ascend, or "progression" is meaningless: a next-mark
  // below the first-mark would make recovery EASIER than starting, which
  // reads as a bug to every user watching the progress bar.
  thresholds_.next = std::max(thresholds_.next, thresholds_.first);
  thresholds_.last = std::max(thresholds_.last, thresholds_.next);
}

void BufferController::OnBufferingStart() {
  if (buffering_) {
    return;
  }
  buffering_ = true;
}

void BufferController::OnBufferingEnd() {
  if (!buffering_) {
    return;
  }
  buffering_ = false;
  // Saturating advance: kFirst -> kNext -> kLast -> kLast. A stream that
  // starves three times has proven its point.
  if (step_ == Step::kFirst) {
    step_ = Step::kNext;
  } else if (step_ == Step::kNext) {
    step_ = Step::kLast;
  }
}

void BufferController::OnSeekCompleted() {
  step_ = Step::kFirst;
  // A seek that somehow lands mid-cycle closes it: the renderer will report
  // its own post-seek state, and an open cycle here would let the stale
  // tier's mark gate the fresh position.
  buffering_ = false;
}

BufferController::Decision BufferController::Evaluate(
    base::TimeDelta buffered_time) const {
  if (!buffering_) {
    return Decision::kProceed;
  }
  return buffered_time >= current_mark() ? Decision::kProceed
                                         : Decision::kKeepWaiting;
}

int BufferController::progress_percent(base::TimeDelta buffered_time) const {
  const base::TimeDelta mark = current_mark();
  if (mark <= base::TimeDelta()) {
    return 100;
  }
  const double fraction =
      buffered_time.InMillisecondsF() / mark.InMillisecondsF();
  return static_cast<int>(std::clamp(fraction * 100.0, 0.0, 100.0));
}

base::TimeDelta BufferController::current_mark() const {
  switch (step_) {
    case Step::kFirst:
      return thresholds_.first;
    case Step::kNext:
      return thresholds_.next;
    case Step::kLast:
      return thresholds_.last;
  }
  return thresholds_.last;
}

}  // namespace avbase::player
