// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The three sinks ask the same question before every pull, and this is the
// rule they all ask with. The case worth pinning is the one that used to be
// impossible to reach: a sink that is paused AND owes a frame, which is
// exactly what a seek made while paused produces.

#include "media/base/video_sink_gate.h"

#include "gtest/gtest.h"

namespace avbase::media {
namespace {

TEST(VideoSinkGateTest, ASinkThatWasNeverFlushedStaysQuietWhilePaused) {
  VideoSinkGate gate;
  EXPECT_FALSE(gate.ShouldPull());
  gate.SetPlaying(true);
  EXPECT_TRUE(gate.ShouldPull());
  gate.SetPlaying(false);
  EXPECT_FALSE(gate.ShouldPull());
}

TEST(VideoSinkGateTest, AFlushAsksOnceEvenWhilePaused) {
  VideoSinkGate gate;
  gate.SetPlaying(false);
  gate.NoteFlushed();
  EXPECT_TRUE(gate.ShouldPull());
  // Spent by the frame that answers it, and by nothing else: this is the
  // difference between "we asked" and "a new picture arrived".
  gate.NoteFrameReceived();
  EXPECT_FALSE(gate.ShouldPull())
      << "the flush's one-shot ask outlived the frame it was for";
}

TEST(VideoSinkGateTest, ANullAnswerDoesNotSpendTheAsk) {
  // A null answer means "keep showing the previous one", so the frame the
  // flush promised is still decoding and the ask must stand.
  VideoSinkGate gate;
  gate.NoteFlushed();
  ASSERT_TRUE(gate.ShouldPull());
  // Nothing calls NoteFrameReceived() here: Render() answered nullptr.
  EXPECT_TRUE(gate.ShouldPull());
}

TEST(VideoSinkGateTest, PausingDoesNotSwallowTheAsk) {
  // The renderer pauses the sink before flushing it (Flush() is only valid
  // while not playing), so the order is Pause-then-NoteFlushed. The reverse
  // order must not lose the ask either.
  VideoSinkGate paused_first;
  paused_first.SetPlaying(false);
  paused_first.NoteFlushed();
  EXPECT_TRUE(paused_first.ShouldPull());

  VideoSinkGate flush_first;
  flush_first.NoteFlushed();
  flush_first.SetPlaying(false);
  EXPECT_TRUE(flush_first.ShouldPull()) << "a pause spent the flush";
}

TEST(VideoSinkGateTest, APlayingSinkKeepsAskingAfterTheFlushIsSpent) {
  VideoSinkGate gate;
  gate.SetPlaying(true);
  gate.NoteFlushed();
  gate.NoteFrameReceived();
  EXPECT_TRUE(gate.ShouldPull());
}

}  // namespace
}  // namespace avbase::media
