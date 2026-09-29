// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// ALGORITHM PROVENANCE
// --------------------
// ResolveMasterType(), GetMasterClock(), ComputeAudioSampleAdjustment() and
// AlignAudioDurationToVideo() are line-by-line ports of get_master_sync_type(),
// get_master_clock(), synchronize_audio() and synchronize_audio_to_video() from
// ijkmedia/ijkplayer/ff_ffplay.c (LGPL-2.1, Copyright Bilibili / Zhang Rui),
// which derives from ffplay.c (LGPL-2.1, Copyright Fabrice Bellard).
//
// Thresholds are identical by design. Do not tune them without golden-test
// evidence; see docs/01 §6 and docs/07 §7.

#ifndef IJKPP_MEDIA_FILTERS_AV_SYNC_CONTROLLER_H_
#define IJKPP_MEDIA_FILTERS_AV_SYNC_CONTROLLER_H_

#include <stdint.h>

#include <atomic>

#include "base/memory/raw_ptr.h"
#include "base/synchronization/lock.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"
#include "media/base/time_source.h"
#include "media/filters/clock.h"
#include "media/media_export.h"

namespace ijkpp::media {

// Owns the three media clocks and resolves which one is authoritative.
//
// Replaces the loose `audclk`/`vidclk`/`extclk` fields plus `av_sync_type` in
// ijkplayer's VideoState, where any of the three thread functions could read or
// write any of them without synchronisation.
class IJKPP_MEDIA_EXPORT AvSyncController final : public TimeSource {
 public:
  enum class MasterType { kAudio = 0, kVideo, kExternal };

  // Thresholds ported verbatim from ffplay. Values must come from
  // tools/extract_constants.py, not from memory.
  struct Thresholds {
    double audio_diff_avg_coef{0.9};       // AV_DIFF_AVG_COEF
    int audio_diff_avg_count{10};          // AV_DIFF_AVG_NB
    double audio_diff_threshold{0.1};      // AV_DIFF_THRESHOLD
    int sample_correction_percent_max{10}; // SAMPLE_CORRECTION_PERCENT_MAX
    base::TimeDelta no_sync_threshold = base::Seconds(10);  // AV_NOSYNC_THRESHOLD
  };

  struct Snapshot {
    Clock::Snapshot audio;
    Clock::Snapshot video;
    Clock::Snapshot external;
    base::TimeDelta master;
    base::TimeDelta av_diff;
    MasterType requested{MasterType::kAudio};
    MasterType resolved{MasterType::kAudio};
    bool master_valid{false};
  };

  AvSyncController(MasterType master, const base::TickClock* wall_clock,
                   const Thresholds& thresholds);
  AvSyncController(const AvSyncController&) = delete;
  AvSyncController& operator=(const AvSyncController&) = delete;
  ~AvSyncController() override;

  // ---- Writers ----
  // Called by the audio renderer for every buffer handed to the device.
  void OnAudioFramesConsumed(int frames, base::TimeDelta media_time,
                             int32_t serial);
  // Called by the compositor after presenting a frame.
  void OnVideoFramePresented(base::TimeDelta media_time, int32_t serial);
  void SetExternalClock(base::TimeDelta media_time, int32_t serial);
  void SetPlaybackRate(double rate);
  // Declares which streams exist, so master selection can fall back when the
  // requested master has no stream (ffplay's get_master_sync_type does exactly
  // this).
  void SetStreamAvailability(bool has_audio, bool has_video);
  // Called after a seek: drops every clock until fresh samples arrive, so a
  // stale pre-seek timestamp is never used to schedule a post-seek frame.
  void Flush();

  MasterType requested_master() const { return requested_master_; }
  void set_requested_master(MasterType master) { requested_master_ = master; }

  // ---- Readers (thread-safe, lock-free) ----
  // The authoritative media time. Never returns a value from a clock that does
  // not exist; falls back exactly as ffplay does.
  base::TimeDelta GetMasterClock() const;
  int32_t master_serial() const;
  bool master_clock_valid() const;
  // What was actually chosen, after the availability fallback.
  MasterType ResolveMasterType() const;
  base::TimeDelta av_diff() const;

  // Ported from synchronize_audio(): how many samples to add to or drop from
  // the next audio buffer so the audio clock converges on the master. Only
  // active when the master is NOT audio.
  int ComputeAudioSampleAdjustment(int buffer_frames, int sample_rate) const;

  // Ported from synchronize_audio_to_video(): nudges a raw audio duration so
  // audio drifts toward video instead of away from it.
  base::TimeDelta AlignAudioDurationToVideo(base::TimeDelta raw_duration);

  // TimeSource:
  void GetWallClockTimes(const std::vector<base::TimeDelta>& media_timestamps,
                         base::TimeTicks reference_time,
                         std::vector<WallClockTime>* out) override;

  Snapshot GetSnapshot() const;
  const Clock& audio_clock() const { return audio_; }
  const Clock& video_clock() const { return video_; }
  const Clock& external_clock() const { return external_; }

 private:
  // const because it only touches the mutable diff window; called from the
  // const ComputeAudioSampleAdjustment() on the audio sequence.
  void UpdateAudioDiff(base::TimeDelta diff) const;

  const base::raw_ptr<const base::TickClock> wall_;
  const Thresholds thresholds_;
  MasterType requested_master_;

  Clock audio_;
  Clock video_;
  Clock external_;

  // Guards only the audio-diff sliding window, which is written by the audio
  // sequence and read by ComputeAudioSampleAdjustment(). Everything else is
  // lock-free through Clock's seqlock.
  // mutable: the sliding window is updated from the const
  // ComputeAudioSampleAdjustment(), which is const because the audio sequence
  // calls it while holding only a const reference to the controller.
  mutable base::Lock diff_lock_;
  mutable double audio_diff_cum_ GUARDED_BY(diff_lock_){0.0};
  mutable int audio_diff_samples_ GUARDED_BY(diff_lock_){0};
  mutable double audio_diff_avg_ GUARDED_BY(diff_lock_){0.0};

  std::atomic<bool> has_audio_{true};
  std::atomic<bool> has_video_{true};
  std::atomic<float> rate_{1.0f};
};

IJKPP_MEDIA_EXPORT const char* GetMasterTypeName(
    AvSyncController::MasterType type);

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_FILTERS_AV_SYNC_CONTROLLER_H_
