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
// its scheduling algorithm is a line-by-line port of ffplay's video_refresh()
// and compute_target_delay(), which makes it a derivative work of ijkplayer
// (LGPL-2.1). Everything else in avbase is BSD-3-Clause (see the root LICENSE);
// ported files are quarantined in media/filters/legacy/ so the boundary is
// auditable by directory listing rather than by reading every header.
// Do not move non-ported code in here, and do not move these files out without
// a legal review.
//
// ALGORITHM PROVENANCE
// --------------------
// ComputeTargetDelay() and the pacing/drop logic inside DecideNextFrame() are
// a line-by-line port of compute_target_delay() and video_refresh() from
// ijkmedia/ijkplayer/ff_ffplay.c (LGPL-2.1, Copyright Bilibili / Zhang Rui),
// which in turn derives from ffplay.c (LGPL-2.1, Copyright Fabrice Bellard).
//
// Every threshold and every branch below has a comment naming its origin. The
// structure differs — the decision is extracted into a pure static function so
// it can be exhaustively unit tested — but the behaviour is intended to be
// identical, and is verified against the original by tests/golden (docs/07 §7).

#include "media/filters/legacy/video_frame_compositor.h"

#include <algorithm>
#include <cmath>

#include "base/check.h"
#include "base/logging.h"
#include "base/time/default_tick_clock.h"

namespace avbase::media {
namespace {

// Clamps |value| into [lo, hi]. Port of ffplay's av_clip for durations.
base::TimeDelta Clamp(base::TimeDelta value, base::TimeDelta lo,
                      base::TimeDelta hi) {
  if (value < lo)
    return lo;
  if (value > hi)
    return hi;
  return value;
}

// Tolerance so a frame arriving exactly on the cap boundary is not dropped by
// rounding. Must match VideoFrameCompositor::Thresholds::min_sleep.
constexpr base::TimeDelta kFpsCapSlack = base::Microseconds(500);

using Decision = VideoFrameCompositor::Decision;
using DropReason = VideoFrameCompositor::DropReason;
using FrameSyncOutput = VideoFrameCompositor::FrameSyncOutput;

// Both take |out| by value and return it, so the fields DecideNextFrame has
// already computed (target_delay, av_diff, frame_duration) survive into the
// returned decision -- returning a fresh object here silently dropped them,
// and PlaybackRateScalesTargetDelayNotTheHoldGate caught it.
FrameSyncOutput MakeHold(FrameSyncOutput out, base::TimeDelta wait,
                         base::TimeTicks retry_at) {
  out.decision = Decision::kHold;
  out.retry_at = retry_at;
  out.frame_duration = wait;
  return out;
}

FrameSyncOutput MakeDrop(FrameSyncOutput out, DropReason reason) {
  out.decision = Decision::kDrop;
  out.drop_reason = reason;
  return out;
}

// IsNoTimestamp comes from media/base/media_constants.h (pulled in by the
// header for the master_clock_ default): one predicate, not a second copy
// in this TU's anonymous namespace.

}  // namespace

const char* GetDecisionName(VideoFrameCompositor::Decision decision) {
  using D = VideoFrameCompositor::Decision;
  switch (decision) {
  case D::kPresent:
    return "Present";
  case D::kHold:
    return "Hold";
  case D::kDrop:
    return "Drop";
  case D::kRepeat:
    return "Repeat";
  case D::kEndOfStream:
    return "EndOfStream";
  }
  return "Invalid";
}

const char* GetDropReasonName(VideoFrameCompositor::DropReason reason) {
  using R = VideoFrameCompositor::DropReason;
  switch (reason) {
  case R::kNone:
    return "none";
  case R::kStaleSerial:
    return "stale-serial";
  case R::kLateBeyondWindow:
    return "late-beyond-window";
  case R::kFpsCap:
    return "fps-cap";
  case R::kAccurateSeek:
    return "accurate-seek";
  }
  return "invalid";
}

// ---------------------------------------------------------------------------
// Pure decision core
// ---------------------------------------------------------------------------

// static
base::TimeDelta VideoFrameCompositor::ApplyFpsCap(base::TimeDelta duration,
                                                  int max_fps) {
  if (max_fps <= 0) {
    return duration;
  }
  const base::TimeDelta min_interval = base::Microseconds(1000000 / max_fps);
  return duration < min_interval ? min_interval : duration;
}

// static
// Port of the `last_duration` derivation in ffplay's video_refresh(): the pts
// delta, but only when both frames share a serial -- across a seek it spans
// the discontinuity and stalls for seconds (docs/04 §4 rule R6; regression
// test SkipsDurationComputationAcrossSeekSerialChange). Otherwise, and when
// the delta is NaN/<=0/>10, the frame rate's own duration.
base::TimeDelta
VideoFrameCompositor::DeriveFrameDuration(const FrameSyncInput& in,
                                          const Thresholds& th) {
  const bool timestamps_usable =
      !IsNoTimestamp(in.next_timestamp) && !IsNoTimestamp(in.last_timestamp);

  base::TimeDelta duration;
  if (timestamps_usable && in.next_serial == in.last_serial) {
    duration = in.next_timestamp - in.last_timestamp;
  }
  if (!timestamps_usable || duration <= base::TimeDelta() ||
      duration > th.max_sane_frame_duration) {
    duration = in.fps_duration > base::TimeDelta() ? in.fps_duration
                                                   : in.next_duration;
  }
  if (duration <= base::TimeDelta()) {
    // No usable timing information at all. Present immediately rather than
    // spinning; a stream without timestamps cannot be paced.
    duration = th.min_sleep;
  }
  return duration;
}

// static
// Port of compute_target_delay() in ff_ffplay.c, whose `diff` is
// `video_clock - master_clock`. This port works with its negation,
// `av_diff = master_clock - next_timestamp`, so the branches are mirrored:
// positive av_diff means video is behind the master and must hurry up,
// negative means it is ahead and should wait.
//
//   sync_threshold = clip(delay, MIN, MAX);
//   if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
//     if (diff <= -sync_threshold) delay = max(0, delay + diff);
//     else if (diff >= sync_threshold && delay > FRAMEDUP) delay += diff;
//     else if (diff >= sync_threshold) delay = 2 * delay;
//   }
base::TimeDelta
VideoFrameCompositor::ComputeTargetDelay(base::TimeDelta last_duration,
                                         const FrameSyncInput& in,
                                         const Thresholds& th) {
  base::TimeDelta delay = last_duration;

  // ffplay only re-paces video when video is *not* the master clock; when it
  // is the master there is nothing to follow.
  if (in.master_is_video || !in.master_clock_valid ||
      IsNoTimestamp(in.next_timestamp)) {
    return delay;
  }

  const base::TimeDelta av_diff = in.master_clock - in.next_timestamp;
  if (av_diff.is_infinte()) {
    return delay;
  }

  // AV_NOSYNC_THRESHOLD: streams too far apart to be corrected by pacing.
  const base::TimeDelta abs_diff =
      av_diff < base::TimeDelta() ? -av_diff : av_diff;
  if (abs_diff >= th.no_sync_threshold) {
    return delay;
  }

  // sync_threshold scales with the frame interval, clamped to
  // [AV_SYNC_THRESHOLD_MIN, AV_SYNC_THRESHOLD_MAX].
  const base::TimeDelta sync_threshold =
      Clamp(delay, th.av_sync_threshold_min, th.av_sync_threshold_max);

  if (av_diff >= sync_threshold) {
    // Video is behind the master clock: shorten the wait, never below zero.
    delay = std::max(base::TimeDelta(), delay - av_diff);
  } else if (av_diff <= -sync_threshold) {
    if (delay > th.sync_framedup_threshold) {
      // Long frames: absorb the full drift.
      delay = delay - av_diff;  // av_diff is negative, so this lengthens.
    } else {
      // Short frames: ffplay duplicates the frame by doubling the delay.
      delay = delay * 2;
    }
  }

  // Playback rate scales wall-clock waiting: 2x speed waits half as long.
  if (in.playback_rate > 0.0 && in.playback_rate != 1.0) {
    // Explicit widening to double: -Wconversion (debug preset, -Werror) rejects
    // the implicit one. Precision is only lost above 2^53 microseconds, which
    // is not a reachable presentation delay.
    delay = base::Microseconds(static_cast<int64_t>(
        static_cast<double>(delay.InMicroseconds()) / in.playback_rate));
  }
  return delay;
}

// static
//
// Rejection rules, in priority order: a stale-seek frame is dropped even if it
// is also late, and an accurate-seek frame even if it is inside the fps cap.
// Kept separate from the timing arithmetic so both fit the function budget.
VideoFrameCompositor::DropReason
VideoFrameCompositor::ClassifyCandidate(const FrameSyncInput& in) {
  // ffplay: `if (vp->serial != is->videoq.serial) { frame_queue_next(); goto
  // retry; }`
  if (in.next_serial < in.queue_serial) {
    return DropReason::kStaleSerial;
  }

  // ijkplayer's `enable-accurate-seek` drops every frame before the target.
  if (in.accurate_seek_pending && !IsNoTimestamp(in.accurate_seek_target) &&
      !IsNoTimestamp(in.next_timestamp) &&
      in.next_timestamp < in.accurate_seek_target) {
    return DropReason::kAccurateSeek;
  }

  // ijkplayer's "max-fps". Note that this asks about the SOURCE, not about
  // when the sink happened to poll -- see ShouldDropForFpsCap.
  if (ShouldDropForFpsCap(in)) {
    return DropReason::kFpsCap;
  }

  return DropReason::kNone;
}

// static
//
// ijkplayer's "max-fps": rather than lengthening the display interval (which
// would grow A/V drift), a frame closer than one cap interval to its
// predecessor is dropped. The distance is between TIMESTAMPS, not between
// wall-clock instants, and that is the bug this replaced: a sink polls on its
// own cadence, so a batch of frames lands between two polls and each one reads
// the POLL interval as "time since last present". Measured on 30 fps at the
// default cap of 31: frames 33.3 ms apart read 18 ms, and 151 of 162 were
// dropped for being "too soon" -- a 1 fps picture, frames_dropped_late at 0.
bool VideoFrameCompositor::ShouldDropForFpsCap(const FrameSyncInput& in) {
  if (in.max_fps <= 0 || in.last_serial < 0) {
    return false;  // No cap, or nothing presented to compare against.
  }
  if (IsNoTimestamp(in.next_timestamp) || IsNoTimestamp(in.last_timestamp)) {
    return false;  // No timestamps: the source did not state a rate.
  }
  const base::TimeDelta step = in.next_timestamp - in.last_timestamp;
  if (step <= base::TimeDelta()) {
    return false;  // Same or out of order: a repeat, not a rate question.
  }
  const base::TimeDelta min_interval = base::Microseconds(1000000 / in.max_fps);
  return step + kFpsCapSlack < min_interval;
}

// static
VideoFrameCompositor::FrameSyncOutput
VideoFrameCompositor::DecideNextFrame(const FrameSyncInput& in,
                                      const Thresholds& th) {
  FrameSyncOutput out;

  // --- 1. Gates that stop pacing entirely --------------------------------
  // ffplay: `if (is->paused) goto display;` — while paused we neither advance
  // nor drop; the current frame simply stays on screen. |refresh_pending| is
  // the one exception, and it is what "seek while paused" is made of: the
  // clock is stopped but the frame is a different one, so it is shown once
  // and then the gate closes again.
  if (in.buffering_blocked ||
      ((in.paused && !in.step_mode) && !in.refresh_pending)) {
    return MakeHold(out, base::TimeDelta(), in.now + th.max_sleep);
  }

  // --- 2. Nothing to show -------------------------------------------------
  if (!in.has_candidate) {
    if (in.end_of_stream) {
      out.decision = Decision::kEndOfStream;
      return out;
    }
    // ffplay waits on the queue's condition variable; we ask to be retried
    // shortly so the control plane stays responsive.
    return MakeHold(out, base::TimeDelta(), in.now + th.min_sleep);
  }

  // --- 3. Frame-by-frame stepping -----------------------------------------
  // ffplay: `if (is->step) { is->step = 0; ... }` presents exactly one frame.
  if (in.step_mode) {
    out.decision = Decision::kPresent;
    out.frame_duration = in.fps_duration > base::TimeDelta() ? in.fps_duration
                                                             : in.next_duration;
    return out;
  }

  // --- 4. Outright rejections (stale serial, accurate seek, fps cap) --------
  if (const DropReason reason = ClassifyCandidate(in);
      reason != DropReason::kNone) {
    return MakeDrop(out, reason);
  }

  // --- 7. Nominal frame duration ------------------------------------------
  const base::TimeDelta last_duration = DeriveFrameDuration(in, th);
  out.frame_duration = last_duration;

  // --- 8. A/V drift, for statistics and for the drop decision --------------
  if (in.master_clock_valid && !IsNoTimestamp(in.next_timestamp) &&
      !IsNoTimestamp(in.master_clock)) {
    out.av_diff = in.master_clock - in.next_timestamp;
  }

  // --- 9. Pace against the master clock -----------------------------------
  // |target_delay| is how long this frame should occupy the screen; it is
  // reported so the caller can advance frame_timer after presenting.
  out.target_delay = ComputeTargetDelay(last_duration, in, th);

  // A frame a flush asked for is due NOW, whatever pacing says: a seek made
  // while paused stopped the clock, not the picture. It reaches this point
  // only after the rejection rules and after the arithmetic -- deliberately,
  // because |target_delay| is what the NEXT frame is paced against, and an
  // un-paced refresh (target_delay == 0) makes the frame after it due at once:
  // for a secondary source with an offset that is the frame that had to be
  // held.
  if (in.refresh_pending) {
    out.decision = Decision::kPresent;
    return out;
  }

  // A null frame_timer means nothing has ever been presented: ffplay shows the
  // first frame as soon as it is decoded rather than waiting one interval, and
  // so do we -- pacing is meaningless without a reference point.
  if (!in.frame_timer.is_null() && in.frame_timer - in.now > th.min_sleep) {
    // Not yet due -- unless the sink's display window already covers the due
    // instant, in which case presenting now beats waiting for the next
    // refresh (Chromium's VideoRendererSink::RenderCallback contract).
    if (!in.deadline_max.is_null() && in.frame_timer <= in.deadline_max) {
      out.decision = Decision::kPresent;
      return out;
    }
    return MakeHold(out, out.target_delay, in.frame_timer);
  }

  // --- 10. Framedrop: we are past the due time -----------------------------
  if (ShouldDropForLateness(in, out, last_duration, th)) {
    return MakeDrop(out, DropReason::kLateBeyondWindow);
  }

  out.decision = Decision::kPresent;
  return out;
}

// static
//
// ffplay's framedrop block: drop only when video is the slave clock, the drop
// budget is not exhausted, the frame is later than its own duration, and the
// drift is still inside AV_NOSYNC_THRESHOLD -- beyond which the timer is
// snapped instead, because dropping to chase a large drift empties the queue
// without ever catching up.
bool VideoFrameCompositor::ShouldDropForLateness(const FrameSyncInput& in,
                                                 const FrameSyncOutput& out,
                                                 base::TimeDelta frame_duration,
                                                 const Thresholds& th) {
  if (in.max_frame_drop <= 0) {
    return false;  // ijkplayer "framedrop" = 0 or -1 both disable dropping.
  }
  if (in.frames_dropped_since_last_present >= in.max_frame_drop) {
    return false;  // Budget exhausted; present rather than starve the display.
  }
  if (in.master_is_video || !in.master_clock_valid) {
    return false;  // Video is the master, or there is nothing to compare to.
  }
  return out.av_diff > frame_duration && out.av_diff < th.no_sync_threshold;
}

// ---------------------------------------------------------------------------
// Stateful wrapper
// ---------------------------------------------------------------------------

VideoFrameCompositor::VideoFrameCompositor(const Thresholds& thresholds,
                                           const base::TickClock* tick_clock)
    : thresholds_(thresholds),
      tick_clock_(tick_clock ? tick_clock
                             : base::DefaultTickClock::GetInstance()) {
  CHECK(tick_clock_);
}

VideoFrameCompositor::~VideoFrameCompositor() = default;

void VideoFrameCompositor::PutCurrentFrame(
    base::scoped_refptr<VideoFrame> frame) {
  if (!frame) {
    return;
  }
  base::AutoLock scoped(lock_);
  pending_frames_.push_back(std::move(frame));
}

void VideoFrameCompositor::SetMasterClock(base::TimeDelta media_time,
                                          int32_t serial, bool valid) {
  base::AutoLock scoped(lock_);
  master_clock_ = media_time;
  master_serial_ = serial;
  master_clock_valid_ = valid;
}

void VideoFrameCompositor::SetMasterIsVideo(bool master_is_video) {
  base::AutoLock scoped(lock_);
  master_is_video_ = master_is_video;
}

void VideoFrameCompositor::SetPlaybackRate(double rate) {
  base::AutoLock scoped(lock_);
  playback_rate_ = rate;
}

void VideoFrameCompositor::SetMaxFrameDrop(int max_frame_drop) {
  base::AutoLock scoped(lock_);
  max_frame_drop_ = max_frame_drop;
}

void VideoFrameCompositor::SetMaxFps(int max_fps) {
  base::AutoLock scoped(lock_);
  max_fps_ = max_fps;
}

int VideoFrameCompositor::max_fps() const {
  base::AutoLock scoped(lock_);
  return max_fps_;
}

int VideoFrameCompositor::max_frame_drop() const {
  base::AutoLock scoped(lock_);
  return max_frame_drop_;
}

base::TimeDelta VideoFrameCompositor::master_clock() const {
  base::AutoLock scoped(lock_);
  return master_clock_;
}

void VideoFrameCompositor::SetPaused(bool paused) {
  base::AutoLock scoped(lock_);
  paused_ = paused;
}

void VideoFrameCompositor::SetStepMode(bool step) {
  base::AutoLock scoped(lock_);
  step_mode_ = step;
}

void VideoFrameCompositor::SetBufferingBlocked(bool blocked) {
  base::AutoLock scoped(lock_);
  buffering_blocked_ = blocked;
}

void VideoFrameCompositor::BeginAccurateSeek(base::TimeDelta target) {
  base::AutoLock scoped(lock_);
  accurate_seek_pending_ = true;
  accurate_seek_target_ = target;
}

void VideoFrameCompositor::EndAccurateSeek() {
  base::AutoLock scoped(lock_);
  accurate_seek_pending_ = false;
  accurate_seek_target_ = base::TimeDelta();
}

void VideoFrameCompositor::SetFpsDuration(base::TimeDelta duration) {
  base::AutoLock scoped(lock_);
  fps_duration_ = duration;
}

void VideoFrameCompositor::SetEndOfStream() {
  base::AutoLock scoped(lock_);
  end_of_stream_ = true;
}

void VideoFrameCompositor::Flush() {
  base::AutoLock scoped(lock_);
  pending_frames_.clear();
  // Keep current_frame_ so the display does not go black across a seek, but
  // drop all pacing state: scheduling against a pre-seek frame_timer is what
  // causes the "stall for several seconds after seek" bug class.
  frame_timer_ = base::TimeTicks();
  last_present_wall_time_ = base::TimeTicks();
  displayed_duration_ = base::TimeDelta();
  frames_dropped_since_last_present_ = 0;
  end_of_stream_ = false;
  // ...and one present is owed, because the picture the flush is for has not
  // arrived yet -- even if playback is paused, which is why this exists.
  refresh_pending_ = true;
}

size_t VideoFrameCompositor::frames_pending() const {
  base::AutoLock scoped(lock_);
  return pending_frames_.size();
}

bool VideoFrameCompositor::refresh_pending() const {
  base::AutoLock scoped(lock_);
  return refresh_pending_;
}

base::scoped_refptr<VideoFrame> VideoFrameCompositor::current_frame() const {
  base::AutoLock scoped(lock_);
  return current_frame_;
}

void VideoFrameCompositor::PresentLocked(
    const base::scoped_refptr<VideoFrame>& frame, const FrameSyncOutput& out) {
  current_frame_ = frame;
  last_presented_serial_ = frame->serial();
  const base::TimeTicks now = tick_clock_->NowTicks();

  // ffplay's `frame_timer += delay` (and the AV_SYNC_THRESHOLD_MAX snap below
  // it): frame_timer_ holds the instant the NEXT frame is due, so advancing it
  // by the duration just presented is exactly ffplay's invariant. Note the
  // snap only fires when the timer has fallen BEHIND -- there is no cap on how
  // far ahead it may be asked to wait, which is what a frozen picture is.
  frame_timer_ =
      (frame_timer_.is_null() ? now : frame_timer_) + out.target_delay;
  if (out.target_delay > base::TimeDelta() &&
      now - frame_timer_ > thresholds_.av_sync_threshold_max) {
    frame_timer_ = now;  // Snapped: we fell too far behind to catch up.
  }

  last_present_wall_time_ = now;
  displayed_duration_ = out.frame_duration;
  frames_dropped_since_last_present_ = 0;

  frames_presented_.fetch_add(1, std::memory_order_relaxed);
  av_diff_sum_micros_.fetch_add(out.av_diff.InMicroseconds(),
                                std::memory_order_relaxed);
  av_diff_samples_.fetch_add(1, std::memory_order_relaxed);
}

VideoFrameCompositor::FrameSyncInput
VideoFrameCompositor::BuildFrameSyncInputLocked(
    const VideoFrame& candidate, base::TimeTicks deadline_min,
    base::TimeTicks deadline_max) const {
  FrameSyncInput in;
  in.next_timestamp = candidate.timestamp();
  in.next_duration = candidate.duration();
  in.next_serial = candidate.serial();
  in.last_timestamp =
      current_frame_ ? current_frame_->timestamp() : base::TimeDelta();
  in.last_serial = current_frame_ ? current_frame_->serial() : -1;
  in.queue_serial = candidate.serial();
  in.master_clock = master_clock_;
  in.master_serial = master_serial_;
  // A clock from a previous seek generation says nothing about this frame.
  in.master_clock_valid =
      master_clock_valid_ && master_serial_ == in.next_serial;
  in.master_is_video = master_is_video_;
  in.frame_timer = frame_timer_;
  in.now = tick_clock_->NowTicks();
  in.deadline_min = deadline_min;
  in.deadline_max = deadline_max;
  in.last_present_wall_time = last_present_wall_time_;
  in.displayed_duration = displayed_duration_;
  in.fps_duration = fps_duration_;
  in.playback_rate = playback_rate_;
  in.max_frame_drop = max_frame_drop_;
  in.max_fps = max_fps_;
  in.paused = paused_;
  in.step_mode = step_mode_;
  in.buffering_blocked = buffering_blocked_;
  in.refresh_pending = refresh_pending_;
  in.accurate_seek_pending = accurate_seek_pending_;
  in.accurate_seek_target = accurate_seek_target_;
  in.frames_dropped_since_last_present = frames_dropped_since_last_present_;
  in.has_candidate = true;
  in.end_of_stream = end_of_stream_;
  return in;
}

void VideoFrameCompositor::AccountDropLocked(DropReason reason) {
  frames_dropped_.fetch_add(1, std::memory_order_relaxed);
  ++frames_dropped_since_last_present_;
  switch (reason) {
  case DropReason::kLateBeyondWindow:
    frames_dropped_late_.fetch_add(1, std::memory_order_relaxed);
    break;
  case DropReason::kFpsCap:
    frames_dropped_fps_.fetch_add(1, std::memory_order_relaxed);
    break;
  case DropReason::kStaleSerial:
    frames_dropped_stale_serial_.fetch_add(1, std::memory_order_relaxed);
    break;
  case DropReason::kAccurateSeek:
    frames_dropped_accurate_seek_.fetch_add(1, std::memory_order_relaxed);
    break;
  case DropReason::kNone:
    break;
  }
}

base::scoped_refptr<VideoFrame>
VideoFrameCompositor::Render(base::TimeTicks deadline_min,
                             base::TimeTicks deadline_max) {
  base::AutoLock scoped(lock_);

  // Consume at most one frame per call; the sink drives the cadence.
  while (!pending_frames_.empty()) {
    const FrameSyncInput in = BuildFrameSyncInputLocked(
        *pending_frames_.front(), deadline_min, deadline_max);
    const FrameSyncOutput out = DecideNextFrame(in, thresholds_);

    switch (out.decision) {
    case Decision::kPresent: {
      base::scoped_refptr<VideoFrame> frame = pending_frames_.front();
      pending_frames_.pop_front();
      // The picture the flush asked for is on screen, so the refresh is spent
      // -- whichever route let it through (paused gate, step gate, pacing).
      refresh_pending_ = false;
      PresentLocked(frame, out);
      if (step_mode_) {
        step_mode_ = false;  // ffplay: is->step is consumed by one frame.
      }
      return frame;
    }
    case Decision::kDrop:
      pending_frames_.pop_front();
      AccountDropLocked(out.drop_reason);
      DVLOG(2) << "dropped frame reason=" << GetDropReasonName(out.drop_reason);
      continue;  // Evaluate the next candidate within the same call.
    case Decision::kHold:
    case Decision::kRepeat:
    case Decision::kEndOfStream:
      frames_repeated_.fetch_add(1, std::memory_order_relaxed);
      return nullptr;  // Caller keeps displaying the current frame.
    }
  }

  frames_repeated_.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

VideoFrameCompositor::Stats VideoFrameCompositor::GetStats() const {
  Stats stats;
  stats.frames_presented = frames_presented_.load(std::memory_order_relaxed);
  stats.frames_dropped = frames_dropped_.load(std::memory_order_relaxed);
  stats.frames_dropped_late =
      frames_dropped_late_.load(std::memory_order_relaxed);
  stats.frames_dropped_fps =
      frames_dropped_fps_.load(std::memory_order_relaxed);
  stats.frames_dropped_stale_serial =
      frames_dropped_stale_serial_.load(std::memory_order_relaxed);
  stats.frames_dropped_accurate_seek =
      frames_dropped_accurate_seek_.load(std::memory_order_relaxed);
  stats.frames_repeated = frames_repeated_.load(std::memory_order_relaxed);
  const uint64_t samples = av_diff_samples_.load(std::memory_order_relaxed);
  if (samples > 0) {
    stats.avg_av_diff_ms = static_cast<double>(av_diff_sum_micros_.load(
                               std::memory_order_relaxed)) /
                           static_cast<double>(samples) / 1000.0;
  }
  return stats;
}

}  // namespace avbase::media
