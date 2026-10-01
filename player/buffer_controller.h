// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_PLAYER_BUFFER_CONTROLLER_H_
#define IJKPP_PLAYER_BUFFER_CONTROLLER_H_

#include "base/time/time.h"

namespace ijkpp::player {

// The three-tier high-water-mark policy behind PlayerState::kBuffering.
//
// ijkplayer's ffp_check_buffering_l waits for a progressively larger buffer
// after each stall: the first stall only needs a little data to recover
// (fast start), but every subsequent one raises the bar, because a stream
// that starved once will starve again and re-buffering is the most visible
// failure a player has. The three tiers, by ijkplayer's option names:
//
//   first  -- the mark the INITIAL start waits for (fast first frame)
//   next   -- the mark recovery waits for after the first stall
//   last   -- the mark later stalls wait for (and the "full enough" cap)
//
// The progression advances one tier per completed buffering cycle and REWINDS
// to |first| on a seek: the new position knows nothing about the old buffer.
// That rewind is the whole point of the seek reset -- without it, a user who
// seeks into unbuffered territory waits for a "last"-sized fill while staring
// at a frozen frame.
//
// Pure decision core, no threads, no I/O: the owner (PlayerImpl, on the media
// sequence) feeds it buffering transitions and buffered-time snapshots and
// acts on the decisions. All branches are unit-tested; nothing here can
// deadlock, which is exactly why the policy lives apart from the facade.
class BufferController {
 public:
  struct Thresholds {
    base::TimeDelta first = base::Milliseconds(100);
    base::TimeDelta next = base::Milliseconds(500);
    base::TimeDelta last = base::Milliseconds(4000);
  };

  enum class Step { kFirst, kNext, kLast };
  enum class Decision { kKeepWaiting, kProceed };

  // Note: no default argument here -- a nested class's default member
  // initializers are not visible inside the enclosing class's definition.
  explicit BufferController(Thresholds thresholds);
  BufferController();

  // The pipeline reported starvation (BufferingState::kHaveNothing). Begins a
  // buffering cycle at the current tier; a redundant start while already
  // buffering is ignored (the renderer can re-report starvation).
  void OnBufferingStart();

  // The pipeline reported recovery (kHaveEnough). Completes the cycle: the
  // next cycle waits for the next tier up, saturating at |kLast|. A report
  // with no matching start is ignored -- it cannot advance a cycle that never
  // began.
  void OnBufferingEnd();

  // A seek completed. Rewinds the progression to |kFirst| and, if a buffering
  // cycle is somehow still open, closes it: the post-seek world starts fresh.
  void OnSeekCompleted();

  // While buffering: has the current tier's mark been reached?
  Decision Evaluate(base::TimeDelta buffered_time) const;

  // 0..100, how far |buffered_time| is toward the current tier's mark. Used
  // for kBufferingProgress; clamped so a snapshot that races a threshold
  // crossing cannot report 101 or a negative.
  int progress_percent(base::TimeDelta buffered_time) const;

  bool buffering() const { return buffering_; }
  Step step() const { return step_; }
  base::TimeDelta current_mark() const;

 private:
  Thresholds thresholds_;
  Step step_ = Step::kFirst;
  bool buffering_ = false;
};

}  // namespace ijkpp::player

#endif  // IJKPP_PLAYER_BUFFER_CONTROLLER_H_
