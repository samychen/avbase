// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Add/Remove{Video,Audio,Text}Stream are declared on the frozen Pipeline
// interface but describe an axis avbase does not have: `id` would select among
// several streams of one type, and Demuxer::GetStream(type) returns ONE stream
// per type. A second picture is a second URL -- a second Demuxer, or a second
// Player feeding the scene's mailbox -- not a second id.
//
// What this pins is the contract the interface promises for them: a WARNING
// and NO EFFECT, rather than a DCHECK, because a caller may reach them before
// the pipeline is ready. "No effect" is the part that can rot silently -- a
// future implementation that half-worked (say, changed GetVideoStreamId() but
// never re-initialised the renderer) would look like progress and would be
// worse than a clean rejection.

#include "media/filters/pipeline_impl.h"

#include "gtest/gtest.h"

namespace avbase::media {
namespace {

// Deliberately a pipeline that was NEVER STARTED: no demuxer, no renderer, no
// media sequence. That is the state in which the interface comment says a
// caller may legitimately arrive, so it is the state the promise is about.
class PipelineStreamAxisTest : public ::testing::Test {
 protected:
  PipelineImpl pipeline_;
};

TEST_F(PipelineStreamAxisTest, AddStreamHasNoEffect) {
  // -1 means "absent": with no demuxer nothing has been selected.
  ASSERT_EQ(pipeline_.GetVideoStreamId(), -1);
  ASSERT_EQ(pipeline_.GetAudioStreamId(), -1);
  ASSERT_EQ(pipeline_.GetTextStreamId(), -1);
  ASSERT_FALSE(pipeline_.IsRunning());

  pipeline_.AddVideoStream(7);
  pipeline_.AddAudioStream(7);
  pipeline_.AddTextStream(7);

  EXPECT_EQ(pipeline_.GetVideoStreamId(), -1);
  EXPECT_EQ(pipeline_.GetAudioStreamId(), -1);
  EXPECT_EQ(pipeline_.GetTextStreamId(), -1);
  EXPECT_FALSE(pipeline_.IsRunning());
}

TEST_F(PipelineStreamAxisTest, RemoveStreamHasNoEffect) {
  ASSERT_EQ(pipeline_.GetVideoStreamId(), -1);
  ASSERT_FALSE(pipeline_.IsRunning());

  pipeline_.RemoveVideoStream(0);
  pipeline_.RemoveAudioStream(0);
  pipeline_.RemoveTextStream(0);

  EXPECT_EQ(pipeline_.GetVideoStreamId(), -1);
  EXPECT_EQ(pipeline_.GetAudioStreamId(), -1);
  EXPECT_EQ(pipeline_.GetTextStreamId(), -1);
  EXPECT_FALSE(pipeline_.IsRunning());
}

// An id that does not exist must be rejected the same way as one that does:
// there is no id axis to be out of range on, so "0" and "INT_MAX" are equally
// meaningless. This is the case that would start failing the moment somebody
// added a stream vector and left the bounds check to the caller.
TEST_F(PipelineStreamAxisTest, OutOfRangeIdIsRejectedTheSameWay) {
  pipeline_.AddVideoStream(INT_MAX);
  pipeline_.RemoveVideoStream(INT_MAX);

  EXPECT_EQ(pipeline_.GetVideoStreamId(), -1);
  EXPECT_FALSE(pipeline_.IsRunning());
}

}  // namespace
}  // namespace avbase::media
