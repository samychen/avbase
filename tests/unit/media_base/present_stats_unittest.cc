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

TEST(ClassifyFrameOutcomeTest, AFrameTheRendererDidNotHandOverIsADrop) {
  // Render() returning null means "keep showing the previous one" -- no frame
  // was lost, so it is not a failure.
  EXPECT_EQ(ClassifyFrameOutcome(kNoFrame, kNotDiscarding, kReadbackOk,
                                 kUploadFailed),
            FrameOutcome::kDropped);
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
// sink observed, it counted exactly one thing.
TEST(ClassifyFrameOutcomeTest, EveryObservationHasExactlyOneEnding) {
  for (const bool has_frame : {true, false}) {
    for (const bool discard : {true, false}) {
      for (const bool readback_failed : {true, false}) {
        for (const bool presented : {true, false}) {
          PresentStats stats;
          stats.NoteFrameOutcome(ClassifyFrameOutcome(
              has_frame, discard, readback_failed, presented));
          const VideoSinkStats reported = stats.Get();
          EXPECT_EQ(reported.frames_presented + reported.frames_dropped +
                        reported.submit_failures,
                    1u)
              << "has_frame=" << has_frame << " discard=" << discard
              << " readback_failed=" << readback_failed
              << " presented=" << presented;
        }
      }
    }
  }
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
