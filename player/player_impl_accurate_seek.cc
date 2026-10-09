// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// PlayerImpl's accurate-seek sub-state machine: the wait window, its supersede
// and expiry handling, and the completion that fires kAccurateSeekCompleted.
// Split from player_impl_events.cc as a line-count seam (C1): this is a
// self-contained state machine around player::SeekController, and pulling it
// out keeps the events file to "the player answering" the pipeline's callbacks.
// player_impl.h already names this as the accurate-seek TU's home.

#include "player/player_impl.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"

namespace avbase {

void PlayerImpl::BeginAccurateWaitOnMedia(int64_t request_id,
                                         base::TimeDelta target) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(buffer_controller_sequence_);
  // A new wait supersedes a running one: complete the old request as aborted
  // (its caller must never wait forever) and reopen the window for the new
  // target. The old window closes through the same EndAccurateSeek path as
  // every other completion, so no dropped-frames rule can leak.
  if (accurate_seek_.active()) {
    EndAccurateWaitOnMedia();
  }
  accurate_seek_.Begin(request_id, target, config_.seek.accurate_timeout,
                       base::TimeTicks::Now());
  if (pipeline_) {
    pipeline_->BeginAccurateSeek(
        target, base::BindOnce(&PlayerImpl::OnAccurateSeekTargetReached,
                               base::Unretained(this)));
  }
  // Δ10's bound, armed over the whole operation: the keyframe seek, the
  // decode catch-up and the display landing all count against it. The task
  // re-checks the controller state, so completing early makes it a no-op.
  media_thread_.task_runner()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&PlayerImpl::CheckAccurateSeekExpiry,
                     base::Unretained(this)),
      config_.seek.accurate_timeout);
}

void PlayerImpl::EndAccurateWaitOnMedia() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(buffer_controller_sequence_);
  if (!accurate_seek_.active()) {
    return;
  }
  // Superseded by a keyframe seek: close the window first (frames flow
  // again), then answer the caller with kAborted -- "the seek you asked for
  // was replaced", not "the seek failed and you are stuck".
  pipeline_ ? pipeline_->EndAccurateSeek() : (void)0;
  const int64_t id = accurate_seek_.End();
  Player::SeekCB cb;
  base::TimeDelta requested;
  {
    base::AutoLock scoped(seek_lock_);
    auto it = pending_seeks_.find(id);
    if (it != pending_seeks_.end()) {
      cb = std::move(it->second);
      pending_seeks_.erase(it);
    }
    auto at = accurate_seek_targets_.find(id);
    if (at != accurate_seek_targets_.end()) {
      requested = at->second;
      accurate_seek_targets_.erase(at);
    }
  }
  LOG(INFO) << "avbase: accurate seek superseded by a newer seek request";
  SeekCompletedPayload payload;
  payload.request_id = id;
  payload.requested = requested;
  payload.actual = GetMediaTime();
  payload.result = MediaError(
      ErrorCode::kAborted, "accurate seek superseded by a newer seek",
      "request_id = " + std::to_string(id),
      "expected during rapid seeking; match completions to requests by "
      "request_id");
  event_hub_.Post(EventType::kSeekCompleted, std::move(payload),
                  GetMediaTime());
  if (cb) {
    event_hub_.PostClosure(base::BindOnce(
        std::move(cb),
        base::unexpected(MediaError(
            ErrorCode::kAborted, "accurate seek superseded",
            "a newer SeekTo replaced this request before it landed",
            "expected during rapid seeking; re-issue the seek if needed"))));
  }
}

void PlayerImpl::OnAccurateSeekTargetReached() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(buffer_controller_sequence_);
  if (!accurate_seek_.active()) {
    // EndAccurateSeek() also routes a still-pending callback through here;
    // the closed window means "reached" is moot.
    return;
  }
  const player::SeekController::Outcome outcome =
      player::SeekController::Evaluate(true, accurate_seek_.deadline(),
                                       base::TimeTicks::Now());
  if (outcome == player::SeekController::Outcome::kReached) {
    CompleteAccurateSeek(true);
  }
  // kPending cannot happen on a reached hop; kExpired is left to the expiry
  // task so that both completion paths share one code path.
}

void PlayerImpl::CheckAccurateSeekExpiry() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(buffer_controller_sequence_);
  if (!accurate_seek_.active()) {
    return;
  }
  const player::SeekController::Outcome outcome =
      player::SeekController::Evaluate(false, accurate_seek_.deadline(),
                                       base::TimeTicks::Now());
  if (outcome == player::SeekController::Outcome::kExpired) {
    // Δ10: keep playing, make the timeout observable. The close-first order
    // matters -- frames resume before the caller hears anything.
    CompleteAccurateSeek(false);
  }
}

void PlayerImpl::CompleteAccurateSeek(bool reached) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(buffer_controller_sequence_);
  // Capture before End(): End resets the wait state it reads from.
  const base::TimeDelta target = accurate_seek_.target();
  const int64_t id = accurate_seek_.End();
  pipeline_ ? pipeline_->EndAccurateSeek() : (void)0;
  Player::SeekCB cb;
  {
    base::AutoLock scoped(seek_lock_);
    accurate_seek_targets_.erase(id);
    auto it = pending_seeks_.find(id);
    if (it != pending_seeks_.end()) {
      cb = std::move(it->second);
      pending_seeks_.erase(it);
    }
  }
  // The window closed before the events: what the user sees when they react
  // to kAccurateSeekCompleted is exactly what will keep showing.
  AccurateSeekCompletedPayload accurate;
  accurate.position = GetMediaTime();
  event_hub_.Post(EventType::kAccurateSeekCompleted, std::move(accurate),
                  GetMediaTime());
  SeekCompletedPayload payload;
  payload.request_id = id;
  payload.requested = target;
  payload.actual = GetMediaTime();
  if (!reached) {
    payload.result = MediaError(
        ErrorCode::kTimeout,
        "accurate seek timed out before the target "
        "frame was displayed",
        "target = " + target.ToString() +
            "; the keyframe seek landed and "
            "playback continued",
        "raise config.seek.accurate_timeout, or accept the keyframe landing "
        "with SeekMode::kPreviousKeyframe");
  }
  event_hub_.Post(EventType::kSeekCompleted, std::move(payload),
                  GetMediaTime());
  if (cb) {
    event_hub_.PostClosure(base::BindOnce(
        std::move(cb),
        reached ? OkStatus()
                : base::unexpected(
                      MediaError(ErrorCode::kTimeout, "accurate seek timed out",
                                 "the target frame was not displayed within "
                                 "config.seek.accurate_timeout",
                                 "raise config.seek.accurate_timeout, or use "
                                 "SeekMode::kPreviousKeyframe"))));
  }
}

}  // namespace avbase
