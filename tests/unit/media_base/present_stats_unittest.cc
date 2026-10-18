// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/present_stats.h"

#include "gtest/gtest.h"

namespace avbase::media {
namespace {

// The ending a frame gets, spelled as the four things a sink actually observes.
// Ordered as ClassifyFrameOutcome takes them.
constexpr bool kHasFrame = true;
constexpr bool kNoFrame = false;
constexpr bool kDiscarding = true;
constexpr bool kNotDiscarding = false;
constexpr bool kReadbackFailed = true;
constexpr bool kReadbackOk = false;
constexpr bool kUploadSucceeded = true;
constexpr bool kUploadFailed = false;

TEST(ClassifyFrameOutcomeTest, AFrameThatWentUpIsPresented) {
  EXPECT_EQ(ClassifyFrameOutcome(kHasFrame, kNotDiscarding, kReadbackOk,
                                 kUploadSucceeded),
            FrameOutcome::kPresented);
}

TEST(ClassifyFrameOutcomeTest, AnUploadThatRefusedIsASubmitFailure) {
  EXPECT_EQ(ClassifyFrameOutcome(kHasFrame, kNotDiscarding, kReadbackOk,
                                 kUploadFailed),
            FrameOutcome::kSubmitFailure);
}

TEST(ClassifyFrameOutcomeTest, AWindowWithNothingToShowIsNoEvent) {
  // Render() returning null means "keep showing the previous one" -- no frame
  // was lost, so it is neither a drop nor a failure. ffplay draws the same
  // line: video_refresh bumps frame_drops_late on the line that calls
  // frame_queue_next(), so only a frame that had reached the queue can be
  // dropped. An empty queue falls through to `goto display` uncounted.
  EXPECT_EQ(ClassifyFrameOutcome(kNoFrame, kNotDiscarding, kReadbackOk,
                                 kUploadFailed),
            FrameOutcome::kNone);
}

TEST(ClassifyFrameOutcomeTest, DiscardingWithoutAFrameIsAlsoNoEvent) {
  // "discard" means frames handed over are thrown away. Nothing handed over
  // means nothing thrown away, however firmly the sink was told to discard.
  EXPECT_EQ(
      ClassifyFrameOutcome(kNoFrame, kDiscarding, kReadbackOk, kUploadFailed),
      FrameOutcome::kNone);
}

TEST(ClassifyFrameOutcomeTest, DiscardingOutranksTheFrameItAlreadyHas) {
  EXPECT_EQ(
      ClassifyFrameOutcome(kHasFrame, kDiscarding, kReadbackOk, kUploadFailed),
      FrameOutcome::kDropped);
}

// The bug this classification exists to make impossible: a hardware frame that
// could be neither imported nor read back used to be counted as a submit
// failure and then, because the failed readback left the frame null, fall
// through to the "no frame" branch and be counted as a drop too.
TEST(ClassifyFrameOutcomeTest, ReadbackFailureIsCountedOnceNotTwice) {
  EXPECT_EQ(ClassifyFrameOutcome(kNoFrame, kNotDiscarding, kReadbackFailed,
                                 kUploadFailed),
            FrameOutcome::kSubmitFailure);
  // It also outranks a discard: the frame never became displayable, and that
  // is the part worth reporting.
  EXPECT_EQ(ClassifyFrameOutcome(kNoFrame, kDiscarding, kReadbackFailed,
                                 kUploadFailed),
            FrameOutcome::kSubmitFailure);
}

// The invariant, stated as arithmetic rather than as a comment: whatever the
// sink observed, it counted at most one thing -- and exactly one if a frame
// existed (a readback failure leaves |has_frame| false, so it is asked
// separately here; the frame was there, it just could not be read).
TEST(ClassifyFrameOutcomeTest, EveryObservationCountsAtMostOneEnding) {
  for (const bool has_frame : {true, false}) {
    for (const bool discard : {true, false}) {
      for (const bool readback_failed : {true, false}) {
        for (const bool presented : {true, false}) {
          PresentStats stats;
          stats.NoteFrameOutcome(ClassifyFrameOutcome(
              has_frame, discard, readback_failed, presented));
          const VideoSinkStats reported = stats.Get();
          const uint64_t counted = reported.frames_presented +
                                   reported.frames_dropped +
                                   reported.submit_failures;
          const uint64_t expected = (has_frame || readback_failed) ? 1u : 0u;
          EXPECT_EQ(counted, expected)
              << "has_frame=" << has_frame << " discard=" << discard
              << " readback_failed=" << readback_failed
              << " presented=" << presented;
        }
      }
    }
  }
}

// The reason kNone exists, as a rhythm rather than as a single call: a sink
// asks once per display window, so at 30 fps on a 60 Hz display every other ask
// answers "hold". Billed as drops, frames_dropped became a function of the
// refresh rate -- on the Qt host it rose at 60/s while the player sat paused.
TEST(PresentStatsTest, AWindowWithNothingToShowCostsNothing) {
  PresentStats stats;
  for (int window = 0; window < 60; ++window) {
    stats.NoteFrameOutcome(ClassifyFrameOutcome(kNoFrame, kNotDiscarding,
                                                kReadbackOk, kUploadFailed));
  }
  stats.NoteFrameOutcome(FrameOutcome::kPresented);

  const VideoSinkStats reported = stats.Get();
  EXPECT_EQ(reported.frames_presented, 1u);
  EXPECT_EQ(reported.frames_dropped, 0u)
      << "one frame arrived, so one frame is the whole account";
}

TEST(PresentStatsTest, CountsEachEndingInItsOwnField) {
  PresentStats stats;
  stats.NoteFrameOutcome(FrameOutcome::kPresented);
  stats.NoteFrameOutcome(FrameOutcome::kPresented);
  stats.NoteFrameOutcome(FrameOutcome::kDropped);
  stats.NoteFrameOutcome(FrameOutcome::kSubmitFailure);

  const VideoSinkStats reported = stats.Get();
  EXPECT_EQ(reported.frames_presented, 2u);
  EXPECT_EQ(reported.frames_dropped, 1u);
  EXPECT_EQ(reported.submit_failures, 1u);
}

TEST(PresentStatsTest, TimingDoesNotCountAFrame) {
  PresentStats stats;
  PresentTiming timing;
  timing.upload_ms = 0.5;
  timing.compose_ms = 0.1;
  timing.blit_ms = 0.05;
  timing.present_ms = 1.0;
  timing.upload_path = UploadPath::kHwZeroCopy;
  stats.NotePresentTiming(timing);

  // The counter and the timing are two calls on purpose; the timing must not
  // smuggle in a second count of the frame it describes.
  const VideoSinkStats reported = stats.Get();
  EXPECT_EQ(reported.frames_presented, 0u);
  EXPECT_EQ(reported.frames_dropped, 0u);
  EXPECT_EQ(reported.submit_failures, 0u);
  EXPECT_DOUBLE_EQ(reported.render.upload.average_ms(), 0.5);
  EXPECT_EQ(reported.render.upload_path, UploadPath::kHwZeroCopy);
}

}  // namespace
}  // namespace avbase::media
