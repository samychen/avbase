// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/ffmpeg_image_snapshot.h"

#include <cstdio>

#include <gtest/gtest.h>

#include "media/base/video_frame.h"

namespace avbase::media {
namespace {

// A synthetic gradient frame: black frames are valid JPEGs too, but a
// gradient guarantees the entropy encoder produces real bytes.
base::scoped_refptr<VideoFrame> MakeGradientFrame() {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, {64, 48}, {64, 48}, Rational{1, 1}, base::TimeDelta(),
      base::TimeDelta(), 0);
  for (int y = 0; y < 48; ++y) {
    for (int x = 0; x < 64; ++x) {
      frame->mutable_data(VideoFrame::kYPlane)[static_cast<size_t>(y) * 64 +
                                               static_cast<size_t>(x)] =
          static_cast<uint8_t>((x * 4 + y * 5) & 0xff);
    }
  }
  for (int y = 0; y < 24; ++y) {
    for (int x = 0; x < 32; ++x) {
      frame->mutable_data(VideoFrame::kUPlane)[static_cast<size_t>(y) * 32 +
                                               static_cast<size_t>(x)] =
          static_cast<uint8_t>(128 + x);
      frame->mutable_data(VideoFrame::kVPlane)[static_cast<size_t>(y) * 32 +
                                               static_cast<size_t>(x)] =
          static_cast<uint8_t>(128 - x);
    }
  }
  return frame;
}

TEST(ImageSnapshotTest, WritesADecodableJpeg) {
  auto frame = MakeGradientFrame();
  const std::string path = testing::TempDir() + "snapshot_test.jpg";
  const Status status = WriteJpegSnapshot(*frame, path);
  ASSERT_TRUE(status) << status.error().ToString();

  std::FILE* file = std::fopen(path.c_str(), "rb");
  ASSERT_TRUE(file) << "snapshot file missing";
  uint8_t header[3] = {0, 0, 0};
  std::fread(header, 1, 3, file);
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fclose(file);
  std::remove(path.c_str());

  // JPEG SOI marker, and a real payload (not a bare 2-byte SOI).
  EXPECT_EQ(header[0], 0xFF);
  EXPECT_EQ(header[1], 0xD8);
  EXPECT_EQ(header[2], 0xFF);
  EXPECT_GT(size, 200);
}

TEST(ImageSnapshotTest, UnwritablePathIsAnActionableError) {
  auto frame = MakeGradientFrame();
  const Status status =
      WriteJpegSnapshot(*frame, "/nonexistent_dir_for_tests/snap.jpg");
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::kInvalidArgument);
}

}  // namespace
}  // namespace avbase::media
