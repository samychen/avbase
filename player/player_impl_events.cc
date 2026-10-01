// Copyright 2026 The ijkpp Authors. All rights reserved.
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

namespace ijkpp {

void PlayerImpl::OnMediaSeekDone(int64_t request_id,
                                 base::TimeDelta requested) {
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
                                        base::TimeDelta memory_usage) {
  if (state == media::BufferingState::kHaveMetadata) {
    OnPipelineReady();
    return;
  }
  if (state == media::BufferingState::kHaveNothing) {
    event_hub_.Post(EventType::kBufferingStarted, BufferedUpdatePayload{},
                    GetMediaTime());
    return;
  }
  if (state == media::BufferingState::kHaveEnough) {
    event_hub_.Post(EventType::kBufferingEnded, BufferedUpdatePayload{},
                    GetMediaTime());
  }
}

void PlayerImpl::OnWaiting(media::WaitingReason reason) {
  WaitingPayload payload;
  payload.reason = static_cast<WaitingPayload::Reason>(reason);
  event_hub_.Post(EventType::kWaiting, std::move(payload), GetMediaTime());
}

void PlayerImpl::OnStatisticsUpdate(const media::PipelineStatistics& stats) {
  {
    base::AutoLock scoped(snapshot_lock_);
    last_stats_ = stats;
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

}  // namespace ijkpp
