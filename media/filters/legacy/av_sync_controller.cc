// Copyright (c) 2013-2026 Zhang Rui <bbcallen@gmail.com>
// Copyright (c) 2013-2026 Bilibili
// Copyright (c) 2003-2013 Fabrice Bellard (ffplay.c)
// Copyright 2026 The avbase Authors. All rights reserved.
//
// This file is part of avbase.
//
// avbase is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License as published by the Free
// Software Foundation; either version 2.1 of the License, or (at your option)
// any later version.
//
// avbase is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
// details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
//
// ---------------------------------------------------------------------------
// LICENSE: LGPL-2.1-or-later. Full text: media/filters/legacy/LICENSE.LGPL-2.1
//
// WHY THIS FILE IS LGPL AND NOT BSD-3 (decision D10, risk R8, docs/08 §4/§5):
// ResolveMasterType(), GetMasterClock(), ComputeAudioSampleAdjustment() and
// AlignAudioDurationToVideo() are line-by-line ports of ijkplayer's
// get_master_sync_type(), get_master_clock(), synchronize_audio() and
// synchronize_audio_to_video(), which makes this a derivative work
// (LGPL-2.1). The rest of avbase is BSD-3-Clause; see media/filters/legacy/README.md.

#include "media/filters/legacy/av_sync_controller.h"

#include <algorithm>
#include <cmath>

#include "base/check.h"
#include "base/logging.h"
#include "media/base/media_constants.h"

namespace avbase::media {

const char* GetMasterTypeName(AvSyncController::MasterType type) {
  using M = AvSyncController::MasterType;
  switch (type) {
    case M::kAudio:    return "audio";
    case M::kVideo:    return "video";
    case M::kExternal: return "external";
  }
  return "invalid";
}

AvSyncController::AvSyncController(MasterType master,
                                   const base::TickClock* wall_clock,
                                   const Thresholds& thresholds)
    : wall_(wall_clock),
      thresholds_(thresholds),
      requested_master_(master),
      audio_(wall_clock),
      video_(wall_clock),
      external_(wall_clock) {
  CHECK(wall_);
}

AvSyncController::~AvSyncController() = default;

void AvSyncController::OnAudioFramesConsumed(int frames,
                                             base::TimeDelta media_time,
                                             int32_t serial) {
  (void)frames;
  audio_.Set(media_time, serial);
}

void AvSyncController::OnVideoFramePresented(base::TimeDelta media_time,
                                             int32_t serial) {
  video_.Set(media_time, serial);
}

void AvSyncController::SetExternalClock(base::TimeDelta media_time,
                                        int32_t serial) {
  external_.Set(media_time, serial);
}

void AvSyncController::SetPlaybackRate(double rate) {
  rate_.store(static_cast<float>(rate), std::memory_order_release);
  // Re-anchor all three so the rate change takes effect from now, not
  // retroactively across the interval already played.
  audio_.SetSpeed(static_cast<float>(rate));
  video_.SetSpeed(static_cast<float>(rate));
  external_.SetSpeed(static_cast<float>(rate));
}

void AvSyncController::SetStreamAvailability(bool has_audio, bool has_video) {
  has_audio_.store(has_audio, std::memory_order_release);
  has_video_.store(has_video, std::memory_order_release);
}

void AvSyncController::Flush() {
  audio_.Invalidate();
  video_.Invalidate();
  external_.Invalidate();
  base::AutoLock scoped(diff_lock_);
  audio_diff_cum_ = 0.0;
  audio_diff_samples_ = 0;
  audio_diff_avg_ = 0.0;
}

// Port of get_master_sync_type():
//   if (av_sync_type == AV_SYNC_VIDEO_MASTER)
//       return video_st ? VIDEO : AUDIO;
//   else if (av_sync_type == AV_SYNC_AUDIO_MASTER)
//       return audio_st ? AUDIO : VIDEO;
//   else return EXTERNAL;
AvSyncController::MasterType AvSyncController::ResolveMasterType() const {
  const bool audio = has_audio_.load(std::memory_order_acquire);
  const bool video = has_video_.load(std::memory_order_acquire);
  switch (requested_master_) {
    case MasterType::kVideo:
      return video ? MasterType::kVideo : MasterType::kAudio;
    case MasterType::kAudio:
      return audio ? MasterType::kAudio : MasterType::kVideo;
    case MasterType::kExternal:
      return MasterType::kExternal;
  }
  return MasterType::kExternal;
}

// Port of get_master_clock(), including the fallback to the external clock when
// the selected master has no valid timestamp.
base::TimeDelta AvSyncController::GetMasterClock() const {
  const MasterType resolved = ResolveMasterType();
  base::TimeDelta value;
  switch (resolved) {
    case MasterType::kVideo:    value = video_.Get();    break;
    case MasterType::kAudio:    value = audio_.Get();    break;
    case MasterType::kExternal: value = external_.Get(); break;
  }
  if (!media::IsNoTimestamp(value)) {
    return value;
  }
  // ffplay falls through to the external clock when the chosen master's pts is
  // AV_NOPTS_VALUE. Without this, a video-only stream with an audio master
  // would report "no clock" and the compositor would stop pacing entirely.
  return external_.Get();
}

int32_t AvSyncController::master_serial() const {
  switch (ResolveMasterType()) {
    case MasterType::kVideo:    return video_.serial();
    case MasterType::kAudio:    return audio_.serial();
    case MasterType::kExternal: return external_.serial();
  }
  return -1;
}

bool AvSyncController::master_clock_valid() const {
  return !media::IsNoTimestamp(GetMasterClock());
}

base::TimeDelta AvSyncController::av_diff() const {
  const base::TimeDelta audio = audio_.Get();
  const base::TimeDelta video = video_.Get();
  if (media::IsNoTimestamp(audio) || media::IsNoTimestamp(video)) {
    return base::TimeDelta();
  }
  return audio - video;
}

void AvSyncController::UpdateAudioDiff(base::TimeDelta diff) const {
  base::AutoLock scoped(diff_lock_);
  // ffplay:  audio_diff_cum = diff + audio_diff_avg_coef * audio_diff_cum
  audio_diff_cum_ = diff.InSecondsF() +
                    thresholds_.audio_diff_avg_coef * audio_diff_cum_;
  if (audio_diff_samples_ < thresholds_.audio_diff_avg_count) {
    ++audio_diff_samples_;
  } else {
    // ffplay:  avg_diff = audio_diff_cum * (1.0 - audio_diff_avg_coef)
    audio_diff_avg_ = audio_diff_cum_ * (1.0 - thresholds_.audio_diff_avg_coef);
  }
}

// Port of the correction block inside synchronize_audio().
int AvSyncController::ComputeAudioSampleAdjustment(int buffer_frames,
                                                   int sample_rate) const {
  if (buffer_frames <= 0 || sample_rate <= 0) {
    return 0;
  }
  // ffplay only corrects audio when it is the slave clock. When audio is the
  // master there is nothing to follow, and correcting would fight the reference.
  if (ResolveMasterType() == MasterType::kAudio) {
    return 0;
  }
  const base::TimeDelta ref_clock = GetMasterClock();
  if (media::IsNoTimestamp(ref_clock)) {
    return 0;
  }
  const base::TimeDelta audio_now = audio_.Get();
  if (media::IsNoTimestamp(audio_now)) {
    return 0;
  }
  const base::TimeDelta diff = audio_now - ref_clock;
  if (diff > thresholds_.no_sync_threshold || diff < -thresholds_.no_sync_threshold) {
    return 0;   // Too far apart to correct by resampling; snap instead.
  }
  UpdateAudioDiff(diff);

  double avg_diff;
  {
    base::AutoLock scoped(diff_lock_);
    if (audio_diff_samples_ < thresholds_.audio_diff_avg_count) {
      return 0;   // Still filling the window; ffplay does not correct yet.
    }
    avg_diff = audio_diff_avg_;
  }
  if (std::fabs(avg_diff) < thresholds_.audio_diff_threshold) {
    return 0;
  }

  // wanted = frames + diff * sample_rate, clamped to ±sample_correction_percent_max.
  int wanted = buffer_frames +
               static_cast<int>(diff.InSecondsF() * static_cast<double>(sample_rate));
  const int lo = buffer_frames * (100 - thresholds_.sample_correction_percent_max) / 100;
  const int hi = buffer_frames * (100 + thresholds_.sample_correction_percent_max) / 100;
  return std::clamp(wanted, lo, hi) - buffer_frames;
}

// Port of synchronize_audio_to_video(): returns a duration nudged toward the
// video clock so audio converges instead of drifting further.
base::TimeDelta AvSyncController::AlignAudioDurationToVideo(
    base::TimeDelta raw_duration) {
  if (ResolveMasterType() != MasterType::kAudio) {
    return raw_duration;
  }
  const base::TimeDelta audio_now = audio_.Get();
  const base::TimeDelta video_now = video_.Get();
  if (media::IsNoTimestamp(audio_now) || media::IsNoTimestamp(video_now)) {
    return raw_duration;
  }
  const base::TimeDelta diff = audio_now - video_now;
  if (diff > thresholds_.no_sync_threshold || diff < -thresholds_.no_sync_threshold) {
    return raw_duration;
  }
  // ffplay: if audio is behind video, shorten the next buffer so it catches up;
  // if ahead, lengthen it so video can catch up.
  //
  // diff = audio - video, so diff < 0 means audio is behind and the adjustment
  // must be NEGATIVE (a shorter attributed duration makes the audio clock
  // advance faster relative to wall time). Sign error here inverts the whole
  // correction and drives the streams apart instead of together — caught by
  // AlignShortensAudioWhenItIsBehindVideo before any media was ever played.
  //
  // Bounded to the same percentage as the sample correction so a single buffer
  // can never produce an audible jump.
  const int64_t limit = raw_duration.InMicroseconds() *
                        thresholds_.sample_correction_percent_max / 100;
  int64_t adjustment = diff.InMicroseconds();
  adjustment = std::clamp(adjustment, -limit, limit);
  const base::TimeDelta aligned = raw_duration + base::Microseconds(adjustment);
  return aligned.is_negative() ? base::TimeDelta() : aligned;
}

void AvSyncController::GetWallClockTimes(
    const std::vector<base::TimeDelta>& media_timestamps,
    base::TimeTicks reference_time,
    std::vector<WallClockTime>* out) {
  CHECK(out);
  out->clear();
  out->reserve(media_timestamps.size());
  const base::TimeDelta master = GetMasterClock();
  const float rate = rate_.load(std::memory_order_acquire);
  const double scale = rate > 0.0f ? static_cast<double>(rate) : 1.0;
  for (const base::TimeDelta& media_time : media_timestamps) {
    WallClockTime entry;
    entry.media_time = media_time;
    if (media::IsNoTimestamp(master) || media::IsNoTimestamp(media_time)) {
      entry.wall_time = reference_time;
    } else {
      const double offset_seconds =
          (media_time - master).InSecondsF() / scale;
      entry.wall_time = reference_time +
                        base::Microseconds(static_cast<int64_t>(offset_seconds * 1e6));
    }
    out->push_back(entry);
  }
}

AvSyncController::Snapshot AvSyncController::GetSnapshot() const {
  Snapshot snapshot;
  snapshot.audio = audio_.Read();
  snapshot.video = video_.Read();
  snapshot.external = external_.Read();
  snapshot.master = GetMasterClock();
  snapshot.av_diff = av_diff();
  snapshot.requested = requested_master_;
  snapshot.resolved = ResolveMasterType();
  snapshot.master_valid = master_clock_valid();
  return snapshot;
}

}  // namespace avbase::media
