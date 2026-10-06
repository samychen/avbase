// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdio>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "base/test/task_environment.h"
#include "media/base/video_frame.h"
#include "media/filters/ffmpeg_image_snapshot.h"
#include "media/filters/pipeline_impl.h"
#include "tests/support/fake_pipeline_client.h"
#include "tests/support/pipeline_fixture.h"

namespace avbase::media {
namespace {

// TakeSnapshot end to end: real demuxer + decoders, a frame presented by the
// video leg, the compositor grab, and the JPEG encode. The snapshot is
// "the frame on screen when the request lands", so the test plays until the
// sink has rendered, snapshots, and checks the file is a real JPEG.
class PipelineSnapshotTest : public PipelineTestFixture {
 protected:
  PipelineSnapshotTest() : PipelineTestFixture(/*ffmpeg_mode=*/true) {
    media_file_ = "small_h264_aac_3s.mp4";
  }
};

TEST_F(PipelineSnapshotTest, SnapshotDuringPlaybackWritesAValidJpeg) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));
  pipeline_->Play();
  auto* video = video_sinks_->last_sink();
  ASSERT_TRUE(video);
  // Frames must be flowing before a snapshot can hold anything.
  ASSERT_TRUE(PumpUntil([&] { return video->frames().size() >= 2; }))
      << "the video leg never presented; events:\n"
      << client_.EventLog();

  std::atomic<bool> done{false};
  MediaError error = MediaError(ErrorCode::kOk, "", "", "");
  base::scoped_refptr<VideoFrame> frame;
  pipeline_->TakeSnapshot(
      base::TimeDelta(),
      base::BindOnce(
          [](std::atomic<bool>* flag, MediaError* err,
             base::scoped_refptr<VideoFrame>* out, MediaError e,
             base::scoped_refptr<VideoFrame> f) {
            *err = std::move(e);
            *out = std::move(f);
            flag->store(true);
          },
          &done, &error, &frame));
  ASSERT_TRUE(PumpUntil([&] { return done.load(); }))
      << "snapshot never completed; events:\n"
      << client_.EventLog();
  EXPECT_FALSE(error) << error.ToString();
  ASSERT_TRUE(frame);

  // Encode through the same call Player::TakeSnapshot uses, and check the
  // JPEG SOI marker + a real payload.
  const std::string path = testing::TempDir() + "pipeline_snapshot.jpg";
  const Status write = WriteJpegSnapshot(*frame, path);
  ASSERT_TRUE(write) << write.error().ToString();
  std::FILE* file = std::fopen(path.c_str(), "rb");
  ASSERT_TRUE(file);
  uint8_t header[3] = {0, 0, 0};
  const size_t read = std::fread(header, 1, 3, file);
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fclose(file);
  std::remove(path.c_str());
  ASSERT_EQ(read, 3u);
  EXPECT_EQ(header[0], 0xFF);
  EXPECT_EQ(header[1], 0xD8);
  EXPECT_EQ(header[2], 0xFF);
  EXPECT_GT(size, 200);
}

TEST_F(PipelineSnapshotTest, SnapshotOnAnIdlePipelineReportsTheState) {
  // No Play(): the compositor holds nothing, which must come back as a
  // precise error rather than a hang or a null-deref.
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));

  std::atomic<bool> done{false};
  MediaError error = MediaError(ErrorCode::kOk, "", "", "");
  pipeline_->TakeSnapshot(
      base::TimeDelta(),
      base::BindOnce(
          [](std::atomic<bool>* flag, MediaError* err, MediaError e,
             base::scoped_refptr<VideoFrame> f) {
            *err = std::move(e);
            EXPECT_FALSE(f);
            flag->store(true);
          },
          &done, &error));
  ASSERT_TRUE(PumpUntil([&] { return done.load(); }));
  EXPECT_TRUE(error);
}

}  // namespace
}  // namespace avbase::media
