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
// (LGPL-2.1). The rest of avbase is BSD-3-Clause; see
// media/filters/legacy/README.md.
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

#ifndef AVBASE_MEDIA_FILTERS_LEGACY_AV_SYNC_CONTROLLER_H_
#define AVBASE_MEDIA_FILTERS_LEGACY_AV_SYNC_CONTROLLER_H_

#include <stdint.h>

#include <atomic>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/synchronization/lock.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"
#include "media/base/time_source.h"
#include "media/filters/legacy/clock.h"
#include "media/media_export.h"

namespace avbase::media {

// Owns the three media clocks and resolves which one is authoritative.
//
// Replaces the loose `audclk`/`vidclk`/`extclk` fields plus `av_sync_type` in
// ijkplayer's VideoState, where any of the three thread functions could read or
// write any of them without synchronisation.
class AVBASE_MEDIA_EXPORT AvSyncController final : public TimeSource {
 public:
  enum class MasterType { kAudio = 0, kVideo, kExternal };

  // Thresholds ported verbatim from ffplay. Values must come from
  // tools/extract_constants.py, not from memory.
  struct Thresholds {
    double audio_diff_avg_coef{0.9};        // AV_DIFF_AVG_COEF
    int audio_diff_avg_count{10};           // AV_DIFF_AVG_NB
    double audio_diff_threshold{0.1};       // AV_DIFF_THRESHOLD
    int sample_correction_percent_max{10};  // SAMPLE_CORRECTION_PERCENT_MAX
    base::TimeDelta no_sync_threshold =
        base::Seconds(10);  // AV_NOSYNC_THRESHOLD
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

  // ---- Per-source playback offsets (docs/14 stage 2) -----------------------
  // Declares that |source_id| exists. Called once per source at Initialize;
  // re-registering is a no-op that keeps whatever offset was set. Source ids
  // are dense from 0 (the composite numbers its children by position), which
  // is what lets the table below be a plain vector.
  void RegisterSource(int source_id);
  // Maps |media_pts| -- a timestamp in source |source_id|'s OWN timeline --
  // onto the master timeline: the master reading at which a frame carrying
  // that timestamp should be on screen. Source 0 maps identically (the
  // master timeline IS source 0's), and so does any source that was never
  // registered: with no offset stated, a source is assumed time-aligned.
  //
  // This is the quantity a multi-source compositor schedules against
  // (docs/14 §5.1); in stage 2 its callers are the unit tests and the
  // per-leg clock push, which needs the inverse direction (see
  // source_offset). The first production consumer of the forward direction
  // is stage 3's compositor.
  base::TimeDelta SourceTime(int source_id, base::TimeDelta media_pts) const;
  // The offset itself, for the pacing push and for diagnostics. Zero for
  // source 0 and for unregistered sources.
  base::TimeDelta source_offset(int source_id) const;
  // States the offset for a registered source. Source 0's is structurally
  // zero and cannot be changed: the master clock is source 0's audio clock,
  // so shifting source 0 as well would correct the same skew twice. An
  // unregistered id is ignored (a DCHECK in debug builds) -- the renderer
  // registers exactly the sources it discovered at Initialize.
  void SetSourceOffset(int source_id, base::TimeDelta offset);

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

  // The per-source offset table (docs/14 stage 2). Written on the media
  // sequence (Initialize registers; a seek's completion states the offsets)
  // and read from there by the clock push; the lock exists because stage 3's
  // compositor will read SourceTime() from the render sequences, and a
  // plain vector would then be a data race. The table is tiny and touched
  // once per push tick, so the cost is irrelevant next to the three clocks.
  mutable base::Lock source_lock_;
  // Index == source id; size == number of registered sources. Values are
  // offsets, defaulted to zero (time-aligned) by resize.
  std::vector<base::TimeDelta> source_offsets_ GUARDED_BY(source_lock_);

  std::atomic<bool> has_audio_{true};
  std::atomic<bool> has_video_{true};
  std::atomic<float> rate_{1.0f};
};

AVBASE_MEDIA_EXPORT const char*
GetMasterTypeName(AvSyncController::MasterType type);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_LEGACY_AV_SYNC_CONTROLLER_H_
