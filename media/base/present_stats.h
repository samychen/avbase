// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_BASE_PRESENT_STATS_H_
#define AVBASE_MEDIA_BASE_PRESENT_STATS_H_

#include <atomic>
#include <cstdint>

#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "media/base/render_perf_stats.h"
#include "media/base/video_renderer_sink.h"
#include "media/media_export.h"

namespace avbase::media {

// Everything a video sink counts and averages about what it presented.
//
// This exists because there was more than one sink that needed it, and the
// second one had a byte-identical copy: GlVideoSink and Sdl2VideoSink each
// carried the same three atomic counters, the same lock-guarded
// RenderPerfStats, the same present-interval EWMA and the same
// measured_refresh_hz derivation -- and each had its own GetStats() spelling
// them out. Two copies of a counter is a counter that drifts, which is the
// same reason NextFrameDelay() was pulled out of these very sinks.
//
// What is deliberately NOT here: anything about how a frame got to the
// display. The sinks differ in exactly that (a GL presenter versus an
// SDL_Renderer), so this stops at the numbers every sink has in common.
//
// THREADING: the Note* methods run on the sink's render thread; Get() may be
// called from any thread, including the host's while the render thread is
// running.
class AVBASE_MEDIA_EXPORT PresentStats {
 public:
  PresentStats() = default;
  PresentStats(const PresentStats&) = delete;
  PresentStats& operator=(const PresentStats&) = delete;

  // The counter and the timing are two calls because they happen at two
  // places: whether a frame counted as presented is decided where the frame
  // is chosen, while what it cost is only known after the upload ran. Folding
  // them into one call would force the sink to know the timing before it has
  // it -- or to count twice.
  void NotePresented();
  void NotePresentTiming(const PresentTiming& timing);
  void NoteDropped();
  void NoteSubmitFailure();

  // Fills every field of VideoSinkStats: the three counters, the per-stage
  // averages, and the refresh rate derived from the spacing of presents.
  VideoSinkStats Get() const;

 private:
  std::atomic<uint64_t> frames_presented_{0};
  std::atomic<uint64_t> frames_dropped_{0};
  std::atomic<uint64_t> submit_failures_{0};

  // Guards everything below. The counters are atomic because they are bumped
  // on the render thread and read by Get() from anywhere; the aggregates are
  // not atomic because they are updated together and a reader must see one
  // frame's worth of them, not half of two.
  mutable base::Lock lock_;
  RenderPerfStats render_ GUARDED_BY(lock_);
  DurationEwma present_interval_ms_ GUARDED_BY(lock_);
  base::TimeTicks last_present_ticks_ GUARDED_BY(lock_);
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_PRESENT_STATS_H_
