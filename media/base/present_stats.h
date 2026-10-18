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

// What became of one ask.
//
// A frame that ARRIVED has exactly one ending, and carrying the decision as a
// VALUE is what makes that true rather than merely intended. The sinks used to
// report a readback failure and then -- because the failed readback left the
// frame null -- fall into the "no frame" branch and report a drop as well, so
// presented + dropped + failures added up to more frames than the renderer ever
// handed over. Two call sites that must agree on "did this one count already"
// is a rule that gets broken; one function that returns the ending cannot be.
//
// A tick with no frame has NO ending, and that is the distinction kNone exists
// to make. The sink asks once per display window; the compositor answers "keep
// showing what you have" whenever the next frame is not due yet, which at
// 30 fps on a 60 Hz display is every other ask, and constantly while paused.
// Counting those as drops made frames_dropped a measurement of the refresh
// rate rather than of the video.
enum class FrameOutcome {
  kNone,
  kPresented,
  kDropped,
  kSubmitFailure,
};

// Decides an ask's ending from what the sink observed. Pure and free of clock
// and display, so the rule has one definition and can be tested without either.
//
// The order of the questions is the order the sink asked them, and it is the
// answer: a readback failure outranks everything because the frame never
// became displayable and that is the part worth reporting -- hence "a submit
// failure" and not also "a drop". It also leaves the frame null, so it has to
// be asked before |has_frame| or it would read as "nothing arrived".
//
//   |readback_failed|  a hardware frame could be neither imported nor read back
//   |has_frame|        Render() handed over a frame
//   |discard|          the sink was told to throw frames away (seeking, flush)
//   |presented|        the upload and the present both succeeded
//
// "Nothing arrived" is kNone even while discarding: a discarded frame is one
// that existed and was thrown away, not one that never existed.
FrameOutcome ClassifyFrameOutcome(bool has_frame, bool discard,
                                  bool readback_failed, bool presented);

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

  // The one bookkeeping entry point. Taking the ending as a value instead of
  // offering NoteDropped()/NoteSubmitFailure() separately is deliberate: it
  // leaves no way to count one frame twice, which is what the sinks did before
  // (a failed readback was a submit failure and then, via the null frame it
  // left behind, a drop).
  void NoteFrameOutcome(FrameOutcome outcome);

  // Separate from the counter above because it happens elsewhere: whether a
  // frame counted as presented is decided where the frame is chosen, while
  // what it cost is only known after the upload ran. Folding them into one
  // call would force the sink to know the timing before it has it -- or to
  // count twice.
  void NotePresentTiming(const PresentTiming& timing);

  // Fills every field of VideoSinkStats: the three counters, the per-stage
  // averages, and the refresh rate derived from the spacing of presents.
  VideoSinkStats Get() const;

 private:
  void NotePresented();
  void NoteDropped();
  void NoteSubmitFailure();

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
