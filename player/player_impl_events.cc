// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// PlayerImpl's outward-facing half: the seek-completion callback, the runtime
// controls (volume, mute, rate, loop), observer registration, the
// Pipeline::Client callbacks that feed the event hub, and the state accessors
// the facade forwards. player_impl.cc keeps construction, source selection,
// prepare and transport; the split is a line-count one (C1), and the seam is
// "the player answering" versus "the player being driven".

#include "player/player_impl.h"

#include <algorithm>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "media/base/media_constants.h"

namespace avbase {

void PlayerImpl::OnMediaSeekDone(int64_t request_id,
                                 base::TimeDelta requested) {
  // The fresh position knows nothing about the old buffer: the progression
  // rewinds to kFirst, and a cycle left open by the seek's starvation closes
  // (BufferController::OnSeekCompleted's contract).
  DCHECK_CALLED_ON_VALID_SEQUENCE(buffer_controller_sequence_);
  buffer_controller_.OnSeekCompleted();
  bool accurate = false;
  base::TimeDelta accurate_target;
  {
    base::AutoLock scoped(seek_lock_);
    auto at = accurate_seek_targets_.find(request_id);
    if (at != accurate_seek_targets_.end()) {
      accurate = true;
      accurate_target = at->second;
      // The entry stays until CompleteAccurateSeek: the callback must not
      // run while the display is still catching up to the target.
    }
  }
  if (accurate) {
    // The keyframe part is done and decoding has restarted; the drop window
    // is open, so pre-target frames never reach the display. The deadline
    // was armed when the wait began; nothing to do here but wait for the
    // renderer's reached hop or the expiry task.
    return;
  }
  Player::SeekCB cb;
  {
    base::AutoLock scoped(seek_lock_);
    auto it = pending_seeks_.find(request_id);
    if (it != pending_seeks_.end()) {
      cb = std::move(it->second);
      pending_seeks_.erase(it);
    }
  }
  SeekCompletedPayload payload;
  payload.request_id = request_id;
  payload.requested = requested;
  payload.actual = GetMediaTime();
  event_hub_.Post(EventType::kSeekCompleted, std::move(payload),
                  GetMediaTime());
  if (cb) {
    event_hub_.PostClosure(
        base::BindOnce(std::move(cb), OkStatus()));
  }
}

void PlayerImpl::SetPlaybackRate(double rate) {
  rate = std::clamp(rate, media::kMinPlaybackRate, media::kMaxPlaybackRate);
  playback_rate_.store(rate);
  if (pipeline_) {
    pipeline_->SetPlaybackRate(rate);
  }
}

void PlayerImpl::SetVolume(double volume) {
  volume_.store(std::clamp(volume, 0.0, 1.0));
  ApplyGain();
}

void PlayerImpl::SetMuted(bool muted) {
  muted_.store(muted);
  ApplyGain();
}

void PlayerImpl::ApplyGain() {
  if (!pipeline_) {
    return;
  }
  // Mute scales the samples at the device path (Δ13), it does not stop the
  // clock: the audio master stays authoritative while muted.
  pipeline_->SetVolume(static_cast<float>(
      muted_.load() ? 0.0 : volume_.load()));
}

void PlayerImpl::SetLoopCount(int count) {
  loop_count_.store(count);
}

int PlayerImpl::AddObserver(PlayerObserver* observer) {
  return event_hub_.AddObserver(observer);
}

void PlayerImpl::RemoveObserver(int id) {
  event_hub_.RemoveObserver(id);
}

// ---- Pipeline::Client (media sequence) --------------------------------------

void PlayerImpl::OnError(MediaError error) {
  {
    base::AutoLock scoped(state_lock_);
    machine_.TransitionTo(PlayerState::kError);
  }
  prepared_event_.Signal();
  event_hub_.PostError(std::move(error), GetMediaTime());
}

void PlayerImpl::OnEnded() {
  const int count = loop_count_.load();
  if (count < 0 || count > 1) {
    if (count > 0) {
      loop_count_.store(count - 1);
    }
    if (pipeline_) {
      pipeline_->Seek(base::TimeDelta(),
                      base::BindOnce(
                          [](PlayerImpl* self) {
                            if (self->pipeline_) {
                              self->pipeline_->Play();
                            }
                          },
                          base::Unretained(this)));
    }
    return;
  }
  PlayerState previous;
  {
    base::AutoLock scoped(state_lock_);
    previous = machine_.state();
    machine_.TransitionTo(PlayerState::kCompleted);
  }
  event_hub_.Post(EventType::kCompleted, CompletedPayload{}, GetMediaTime());
  event_hub_.PostStateChanged(previous, PlayerState::kCompleted,
                              GetMediaTime());
}

void PlayerImpl::OnDurationChange(base::TimeDelta duration) {
  base::AutoLock scoped(snapshot_lock_);
  media_info_.duration = duration;
}

void PlayerImpl::OnBufferingStateChange(media::BufferingState state,
                                        base::TimeDelta /*memory_usage*/) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(buffer_controller_sequence_);
  if (state == media::BufferingState::kHaveMetadata) {
    OnPipelineReady();
    return;
  }
  if (state == media::BufferingState::kHaveNothing) {
    buffer_controller_.OnBufferingStart();
    event_hub_.Post(EventType::kBufferingStarted, BufferedUpdatePayload{},
                    GetMediaTime());
    return;
  }
  if (state == media::BufferingState::kHaveEnough) {
    // Complete the cycle BEFORE rendering the payload, so the mark it
    // carries is the tier this cycle was waiting on, not the next one.
    const base::TimeDelta waited_for = buffer_controller_.current_mark();
    buffer_controller_.OnBufferingEnd();
    BufferedUpdatePayload payload;
    payload.cached_time = GetBufferedTime();
    payload.high_water_mark_time = waited_for;
    event_hub_.Post(EventType::kBufferingEnded, std::move(payload),
                    GetMediaTime());
  }
}

void PlayerImpl::OnWaiting(media::WaitingReason reason) {
  WaitingPayload payload;
  payload.reason = static_cast<WaitingPayload::Reason>(reason);
  event_hub_.Post(EventType::kWaiting, std::move(payload), GetMediaTime());
}

void PlayerImpl::OnStatisticsUpdate(const media::PipelineStatistics& stats) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(buffer_controller_sequence_);
  {
    base::AutoLock scoped(snapshot_lock_);
    last_stats_ = stats;
  }
  // While a buffering cycle is open, report progress toward the CURRENT
  // tier's mark (M9's observable hwm_step). The pipeline's own
  // kHaveEnough ends the cycle; this tick only re-draws the bar.
  if (buffer_controller_.buffering()) {
    const base::TimeDelta buffered = GetBufferedTime();
    BufferingProgressPayload payload;
    payload.buffered_ahead = buffered;
    payload.percent = buffer_controller_.progress_percent(buffered);
    event_hub_.Post(EventType::kBufferingProgress, std::move(payload),
                    GetMediaTime());
  }
  StatsPayload payload;
  payload.stats = GetPlaybackStats();
  event_hub_.Post(EventType::kStats, std::move(payload), GetMediaTime());
}

void PlayerImpl::OnVideoConfigChange(const media::VideoDecoderConfig& config) {
  {
    base::AutoLock scoped(snapshot_lock_);
    video_coded_size_ = config.coded_size;
    video_natural_size_ = config.natural_size;
  }
  VideoSizeChangedPayload payload;
  payload.coded_size = config.coded_size;
  payload.natural_size = config.natural_size;
  event_hub_.Post(EventType::kVideoSizeChanged, std::move(payload),
                  GetMediaTime());
}

// ---- Queries ----------------------------------------------------------------

PlayerState PlayerImpl::state() const {
  base::AutoLock scoped(state_lock_);
  return machine_.state();
}

std::optional<MediaInfo> PlayerImpl::media_info() const {
  base::AutoLock scoped(snapshot_lock_);
  if (media_info_.streams.empty() && media_info_.format_name.empty()) {
    return std::nullopt;
  }
  return media_info_;
}

base::TimeDelta PlayerImpl::GetMediaTime() const {
  return pipeline_ ? pipeline_->GetMediaTime() : base::TimeDelta();
}

base::TimeDelta PlayerImpl::GetBufferedTime() const {
  return pipeline_ ? pipeline_->GetBufferedTime() : base::TimeDelta();
}

base::TimeDelta PlayerImpl::GetDuration() const {
  return pipeline_ ? pipeline_->GetDuration() : base::TimeDelta();
}

bool PlayerImpl::is_live() const {
  base::AutoLock scoped(snapshot_lock_);
  return is_live_;
}

bool PlayerImpl::IsPlaying() const {
  base::AutoLock scoped(state_lock_);
  return machine_.state() == PlayerState::kStarted;
}

media::Size PlayerImpl::video_natural_size() const {
  base::AutoLock scoped(snapshot_lock_);
  return video_natural_size_;
}

media::Size PlayerImpl::video_coded_size() const {
  base::AutoLock scoped(snapshot_lock_);
  return video_coded_size_;
}

int PlayerImpl::video_rotation() const {
  base::AutoLock scoped(snapshot_lock_);
  return video_rotation_;
}

PlaybackStats PlayerImpl::GetPlaybackStats() const {
  PlaybackStats stats;
  stats.playback_rate = playback_rate_.load();
  stats.volume = muted_.load() ? 0.0 : volume_.load();
  base::AutoLock scoped(snapshot_lock_);
  stats.av_diff = base::SecondsD(last_stats_.avg_av_diff_ms / 1000.0);
  stats.video.cached_duration = last_stats_.buffered_time;
  return stats;
}

std::string PlayerImpl::DumpDiagnostics() const {
  const PlayerState s = state();
  const media::Size natural = video_natural_size();
  std::string json = "{";
  json += "\"state\":\"" + std::string(GetPlayerStateName(s)) + "\"";
  json += ",\"media_time_us\":" +
          std::to_string(GetMediaTime().InMicroseconds());
  json += ",\"duration_us\":" + std::to_string(GetDuration().InMicroseconds());
  json += ",\"buffered_us\":" +
          std::to_string(GetBufferedTime().InMicroseconds());
  json += ",\"rate\":" + std::to_string(playback_rate_.load());
  json += ",\"volume\":" + std::to_string(volume_.load());
  json += ",\"video\":" + std::to_string(natural.width) + "x" +
          std::to_string(natural.height);
  json += "}";
  return json;
}

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
        target,
        base::BindOnce(&PlayerImpl::OnAccurateSeekTargetReached,
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
        ErrorCode::kTimeout, "accurate seek timed out before the target "
                             "frame was displayed",
        "target = " + target.ToString() + "; the keyframe seek landed and "
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
                : base::unexpected(MediaError(
                      ErrorCode::kTimeout, "accurate seek timed out",
                      "the target frame was not displayed within "
                          "config.seek.accurate_timeout",
                      "raise config.seek.accurate_timeout, or use "
                      "SeekMode::kPreviousKeyframe"))));
  }
}

}  // namespace avbase
