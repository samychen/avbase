// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Structure mirrors Chromium's `media::VideoFrameCompositor`
// (BSD-3-Clause, now at third_party/blink/renderer/platform/media/).
//
// The scheduling *algorithm* is a line-by-line port of ijkplayer's
// video_refresh() + compute_target_delay() from
// ijkmedia/ijkplayer/ff_ffplay.c (LGPL-2.1, Copyright Bilibili / Zhang Rui).
// Thresholds are intentionally identical. Do not "improve" them without
// golden-test evidence; see docs/01 §6 and docs/07 §7.

#ifndef IJKPP_MEDIA_FILTERS_VIDEO_FRAME_COMPOSITOR_H_
#define IJKPP_MEDIA_FILTERS_VIDEO_FRAME_COMPOSITOR_H_

#include <stdint.h>

#include <atomic>
#include <cstdint>
#include <deque>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "base/time/tick_clock.h"
#include "media/base/video_frame.h"
#include "media/media_export.h"

namespace ijkpp::media {

// Decides which VideoFrame should be displayed at a given wall-clock instant.
//
// Design note — why DecideNextFrame() is a static pure function
// -------------------------------------------------------------
// ijkplayer keeps the same logic inline in video_refresh(): 400 lines with
// three `goto retry` edges that jump across local variable initialisation, and
// mutable state (`is->frame_timer`, `is->last_duration`, `is->dropped`) living
// in the god-struct VideoState. It cannot be unit tested, because running it
// requires a real clock, a real media file and a real display.
//
// Here the decision is a pure function of an immutable input snapshot:
// no side effects, no allocation, no logging, no I/O, no clock access. That
// makes it exhaustively testable with a fixed input (see
// video_frame_compositor_unittest.cc) while the *stateful* parts — frame_timer,
// the pending frame deque, statistics — live in this class and are trivially
// thin wrappers around it.
//
// Threading: the media sequence writes (PutCurrentFrame, SetMasterClock, ...);
// the sink's render sequence reads through Render(). Shared state is guarded
// by |lock_| and deliberately kept to a small, fixed set of fields.
class IJKPP_MEDIA_EXPORT VideoFrameCompositor {
 public:
  enum class Decision {
    kPresent,      // Display the candidate frame now.
    kDrop,         // Candidate is unusable; discard it and evaluate the next.
    kHold,         // Not yet time; retry at FrameSyncOutput::retry_at.
    kRepeat,       // No new frame available; keep displaying the current one.
    kEndOfStream,  // Queue drained and the decoder signalled EOS.
  };

  // Why a decision was made. Populated only in DCHECK-enabled builds; in
  // release builds it is a string literal with no formatting cost.
  enum class DropReason {
    kNone = 0,
    kStaleSerial,       // Frame predates the current seek generation.
    kLateBeyondWindow,  // Master clock has moved past the frame's window.
    kFpsCap,            // Output rate capped by config.video.max_fps.
    kAccurateSeek,      // Frame is before the accurate-seek target.
  };

  struct FrameSyncInput {
    // Candidate frame and its predecessor, in media time.
    base::TimeDelta next_timestamp;
    base::TimeDelta last_timestamp;
    base::TimeDelta next_duration;        // Per-frame duration from the container.
    base::TimeDelta fps_duration;         // 1 / avg_frame_rate, used as fallback.
    int32_t next_serial{0};
    int32_t last_serial{0};
    int32_t queue_serial{0};

    // Master clock, already resolved by AvSyncController.
    base::TimeDelta master_clock;
    int32_t master_serial{0};
    bool master_clock_valid{false};
    bool master_is_video{false};

    // Scheduling state (wall clock domain).
    // Wall-clock instant at which the NEXT frame becomes due, or null when
    // nothing has been presented yet. ffplay calls this `is->frame_timer` and
    // advances it by the duration of the frame just presented; the same
    // invariant is kept here but stated explicitly, because getting it wrong
    // either stalls the first frame or double-counts every interval.
    base::TimeTicks frame_timer;
    base::TimeTicks now;
    // Display window reported by the VideoRendererSink, if any. When the sink
    // can supply a real vsync window (X11 Present / Wayland
    // wp_presentation_feedback), a frame that becomes due inside it is
    // presented immediately instead of being held for the next refresh.
    // A null deadline_max means "no window information".
    base::TimeTicks deadline_min;
    base::TimeTicks deadline_max;
    base::TimeTicks last_present_wall_time;
    base::TimeDelta displayed_duration;    // Duration of the frame on screen.

    // Playback state.
    double playback_rate{1.0};
    int max_frame_drop{0};
    int max_fps{0};
    bool paused{false};
    bool step_mode{false};
    bool buffering_blocked{false};
    bool accurate_seek_pending{false};
    base::TimeDelta accurate_seek_target;
    int frames_dropped_since_last_present{0};
    bool has_candidate{true};
    bool end_of_stream{false};
  };

  struct FrameSyncOutput {
    Decision decision{Decision::kHold};
    base::TimeDelta frame_duration;   // Duration this frame should be on screen.
    base::TimeDelta target_delay;     // compute_target_delay() result.
    base::TimeDelta av_diff;          // master_clock - video_clock, for stats.
    base::TimeTicks retry_at;         // Valid when decision == kHold.
    DropReason drop_reason{DropReason::kNone};
  };

  // Thresholds ported verbatim from ffplay / ijkplayer. Values must be
  // extracted from ff_ffplay_def.h by tools/extract_constants.py, not typed by
  // hand. See docs/05 table 7.
  struct Thresholds {
    // AV_SYNC_THRESHOLD_MIN / MAX: below MIN do not correct, above MAX do not
    // wait — resynchronise the frame timer instead.
    base::TimeDelta av_sync_threshold_min = base::Milliseconds(40);   // 0.04
    base::TimeDelta av_sync_threshold_max = base::Milliseconds(100);  // 0.1
    // AV_NOSYNC_THRESHOLD: beyond this the streams are too far apart to
    // correct by pacing; snap instead.
    base::TimeDelta no_sync_threshold = base::Seconds(10);            // 10.0
    // AV_SYNC_FRAMEDUP_THRESHOLD: only stretch a delay by the full diff when
    // the frame interval already exceeds this, otherwise double it.
    base::TimeDelta sync_framedup_threshold = base::Milliseconds(10); // 0.1 in ffplay
    // MAX_SLEEP: never sleep longer than this in one go, so that control
    // messages stay responsive.
    base::TimeDelta max_sleep = base::Seconds(1);
    // Below this, waiting is pointless; present immediately.
    base::TimeDelta min_sleep = base::Microseconds(500);
    // Reject a container-provided inter-frame delta larger than this and fall
    // back to the fps-derived duration. ffplay uses 10 seconds.
    base::TimeDelta max_sane_frame_duration = base::Seconds(10);
  };

  struct Stats {
    uint64_t frames_presented{0};
    uint64_t frames_dropped{0};
    uint64_t frames_dropped_late{0};
    uint64_t frames_dropped_fps{0};
    uint64_t frames_dropped_stale_serial{0};
    uint64_t frames_dropped_accurate_seek{0};
    uint64_t frames_repeated{0};
    double avg_av_diff_ms{0.0};
  };

  VideoFrameCompositor(const Thresholds& thresholds,
                       const base::TickClock* tick_clock);
  VideoFrameCompositor(const VideoFrameCompositor&) = delete;
  VideoFrameCompositor& operator=(const VideoFrameCompositor&) = delete;
  ~VideoFrameCompositor();

  // ---- Called on the media sequence -------------------------------------
  void PutCurrentFrame(base::scoped_refptr<VideoFrame> frame);
  void SetMasterClock(base::TimeDelta media_time, int32_t serial, bool valid);
  void SetMasterIsVideo(bool master_is_video);
  void SetPlaybackRate(double rate);
  void SetMaxFrameDrop(int max_frame_drop);
  void SetMaxFps(int max_fps);
  void SetPaused(bool paused);
  void SetStepMode(bool step);
  void SetBufferingBlocked(bool blocked);
  void BeginAccurateSeek(base::TimeDelta target);
  void EndAccurateSeek();
  void SetFpsDuration(base::TimeDelta duration);
  void SetEndOfStream();
  // Called after a seek: invalidates pacing state so the first frame of the
  // new generation is presented immediately rather than scheduled against a
  // stale frame_timer.
  void Flush();

  size_t frames_pending() const;
  base::scoped_refptr<VideoFrame> current_frame() const;

  // ---- Called on the sink's render sequence ------------------------------
  // Picks the frame that should be visible inside [deadline_min, deadline_max].
  // Returns nullptr when the caller should keep displaying the previous frame.
  base::scoped_refptr<VideoFrame> Render(base::TimeTicks deadline_min,
                                   base::TimeTicks deadline_max);

  Stats GetStats() const;

  // ---- Pure decision core -------------------------------------------------
  // Static, side-effect free, allocation free, clock free. This is the single
  // most test-sensitive function in the project; see docs/07 §3.3.
  static FrameSyncOutput DecideNextFrame(const FrameSyncInput& in,
                                         const Thresholds& th);

  // Exposed for unit testing; all are pure.
  //
  // ClassifyCandidate() handles everything that can reject a frame outright
  // (pause, stale serial, accurate seek, fps cap); DecideNextFrame() then does
  // the timing arithmetic. Splitting them keeps each function inside the
  // 80-line budget of invariant C2 and makes the rejection rules readable on
  // their own. Returns kNone when the frame is acceptable.
  static DropReason ClassifyCandidate(const FrameSyncInput& in);
  // Nominal on-screen duration for the candidate, ported from ffplay's
  // `last_duration` derivation including the serial guard.
  static base::TimeDelta DeriveFrameDuration(const FrameSyncInput& in,
                                             const Thresholds& th);
  // The "we are too far behind to be worth showing" test from ffplay's
  // framedrop block.
  static bool ShouldDropForLateness(const FrameSyncInput& in,
                                    const FrameSyncOutput& out,
                                    base::TimeDelta frame_duration,
                                    const Thresholds& th);
  //
  // Adjusts |last_duration| so that video pacing follows the master clock.
  // Port of compute_target_delay() from ff_ffplay.c.
  static base::TimeDelta ComputeTargetDelay(base::TimeDelta last_duration,
                                            const FrameSyncInput& in,
                                            const Thresholds& th);
  // Port of ijkplayer's "max-fps" frame capping.
  static base::TimeDelta ApplyFpsCap(base::TimeDelta duration, int max_fps);

 private:
  // Assembles the immutable input snapshot for DecideNextFrame(). Extracted so
  // that Render() stays inside the 80-line budget and so the snapshot's field
  // mapping is reviewable in one place.
  FrameSyncInput BuildFrameSyncInputLocked(const VideoFrame& candidate,
                                           base::TimeTicks deadline_min,
                                           base::TimeTicks deadline_max) const;
  void PresentLocked(const base::scoped_refptr<VideoFrame>& frame,
                     const FrameSyncOutput& out);
  void AccountDropLocked(DropReason reason);

  const Thresholds thresholds_;
  const base::raw_ptr<const base::TickClock> tick_clock_;

  mutable base::Lock lock_;
  std::deque<base::scoped_refptr<VideoFrame>> pending_frames_ GUARDED_BY(lock_);
  base::scoped_refptr<VideoFrame> current_frame_ GUARDED_BY(lock_);

  base::TimeDelta master_clock_ GUARDED_BY(lock_);
  int32_t master_serial_ GUARDED_BY(lock_){0};
  bool master_clock_valid_ GUARDED_BY(lock_){false};
  bool master_is_video_ GUARDED_BY(lock_){false};

  double playback_rate_ GUARDED_BY(lock_){1.0};
  int max_frame_drop_ GUARDED_BY(lock_){0};
  int max_fps_ GUARDED_BY(lock_){0};
  bool paused_ GUARDED_BY(lock_){false};
  bool step_mode_ GUARDED_BY(lock_){false};
  bool buffering_blocked_ GUARDED_BY(lock_){false};
  bool accurate_seek_pending_ GUARDED_BY(lock_){false};
  base::TimeDelta accurate_seek_target_ GUARDED_BY(lock_);
  bool end_of_stream_ GUARDED_BY(lock_){false};

  base::TimeDelta fps_duration_ GUARDED_BY(lock_);
  base::TimeTicks frame_timer_ GUARDED_BY(lock_);
  base::TimeTicks last_present_wall_time_ GUARDED_BY(lock_);
  base::TimeDelta displayed_duration_ GUARDED_BY(lock_);
  int32_t last_presented_serial_ GUARDED_BY(lock_){0};
  int frames_dropped_since_last_present_ GUARDED_BY(lock_){0};

  // Written from two sequences, read from any; plain atomics, no lock.
  std::atomic<uint64_t> frames_presented_{0};
  std::atomic<uint64_t> frames_dropped_{0};
  std::atomic<uint64_t> frames_dropped_late_{0};
  std::atomic<uint64_t> frames_dropped_fps_{0};
  std::atomic<uint64_t> frames_dropped_stale_serial_{0};
  std::atomic<uint64_t> frames_dropped_accurate_seek_{0};
  std::atomic<uint64_t> frames_repeated_{0};
  std::atomic<int64_t> av_diff_sum_micros_{0};
  std::atomic<uint64_t> av_diff_samples_{0};
};

IJKPP_MEDIA_EXPORT const char* GetDecisionName(
    VideoFrameCompositor::Decision decision);
IJKPP_MEDIA_EXPORT const char* GetDropReasonName(
    VideoFrameCompositor::DropReason reason);

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_FILTERS_VIDEO_FRAME_COMPOSITOR_H_
