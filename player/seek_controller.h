// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_SEEK_CONTROLLER_H_
#define AVBASE_PLAYER_SEEK_CONTROLLER_H_

#include "base/time/time.h"
#include "player/public/player_export.h"

namespace avbase {
namespace player {

// The accurate-seek policy (docs/08 M9 DoD: ±1 frame), split the same way as
// BufferController and DecideNextFrame before it: a pure decision core that
// owns no clock and no threads, plus the minimal state the facade drives.
//
// HOW ACCURATE SEEK WORKS. A keyframe seek lands at or before the request;
// landing exactly means decoding past the keyframe and dropping every frame
// before the target. The media layer provides the drop window
// (Renderer::BeginAccurateSeek, backed by the compositor's accurate-seek
// rule) and reports when a frame at or after the target is actually
// presented; this class owns the policy around that wait:
//   * the timeout (config.seek.accurate_timeout) -- on expiry the seek
//     completes anyway and playback continues, with the timeout visible in
//     SeekCompletedPayload.result (Δ10);
//   * the single-slot rule -- a new seek supersedes a running wait, whose
//     callback is completed as aborted rather than dropped;
//   * the completion classification: reached beats expiry, and "pending" is
//     not a completion at all.
//
// Threading: all state lives on the media sequence (PlayerImpl drives it
// from there); Evaluate() is static and pure, so tests need no threads.
class AVBASE_PLAYER_EXPORT SeekController {
 public:
  enum class Outcome { kPending, kReached, kExpired };

  // Pure decision core. |target_reached| says the display caught up; the
  // deadline check only fires when it has not.
  static Outcome Evaluate(bool target_reached, base::TimeTicks deadline,
                          base::TimeTicks now);

  SeekController() = default;
  SeekController(const SeekController&) = delete;
  SeekController& operator=(const SeekController&) = delete;

  bool active() const { return active_; }
  int64_t request_id() const { return request_id_; }
  base::TimeDelta target() const { return target_; }
  // The absolute expiry the caller schedules its timeout against.
  base::TimeTicks deadline() const { return deadline_; }

  // Opens a wait for |request_id| landing on |target|, expiring |timeout|
  // from |now|. Replaces any previous wait (the caller completes the old
  // one before calling this).
  void Begin(int64_t request_id, base::TimeDelta target,
             base::TimeDelta timeout, base::TimeTicks now);
  // Closes the wait and returns the request id it belonged to (-1 if none).
  int64_t End();

 private:
  bool active_{false};
  int64_t request_id_{-1};
  base::TimeDelta target_;
  base::TimeTicks deadline_;
};

}  // namespace avbase::player
}  // namespace avbase

#endif  // AVBASE_PLAYER_SEEK_CONTROLLER_H_
