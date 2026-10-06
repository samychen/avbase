// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/ffmpeg_remuxer.h"

#include <cstdio>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "media/filters/ffmpeg_demuxer.h"

namespace avbase::media {
namespace {

std::string TestFile(const char* name) {
  return std::string(AVBASE_TESTDATA_DIR) + "/" + name;
}

class RecordingHost final : public Demuxer::Host {
 public:
  void SetDuration(base::TimeDelta /*duration*/) override {}
  void OnBufferedTimeUpdate(base::TimeDelta, base::TimeDelta) override {}
  void OnDemuxerError(MediaError error) override { errors.push_back(error); }

  std::vector<MediaError> errors;
};

class RemuxerTest : public ::testing::Test {
 protected:
  // Opens |path| through the regular demuxer; the round-trip check for
  // every remux.
  Status Open(const std::string& path) {
    Status result = Err(ErrorCode::kNotImplemented, "not run", {}, {});
    bool done = false;
    demuxer_->Initialize(DataSourceDescriptor::FromUri(path), {}, &host_,
                         task_environment_.GetMainThreadTaskRunnerRef(),
                         base::BindOnce(
                             [](Status* out, bool* flag, Status s) {
                               *out = std::move(s);
                               *flag = true;
                             },
                             &result, &done));
    for (int i = 0; i < 2000 && !done; ++i) {
      task_environment_.RunUntilIdle();
      if (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    return result;
  }

  base::test::TaskEnvironment task_environment_;
  base::scoped_refptr<MediaLog> media_log_ = base::MakeRefCounted<MediaLog>();
  std::unique_ptr<FFmpegDemuxer> demuxer_ =
      std::make_unique<FFmpegDemuxer>(media_log_);
  RecordingHost host_;
};

TEST_F(RemuxerTest, Mp4ToMkvKeepsStreamsAndDuration) {
  const std::string dst = testing::TempDir() + "remux_test.mkv";
  const Status status = RemuxContainer(TestFile("small_h264_aac_3s.mp4"), dst);
  ASSERT_TRUE(status) << status.error().ToString();

  ASSERT_TRUE(Open(dst));
  const MediaInfo& info = demuxer_->media_info();
  EXPECT_EQ(info.streams.size(), 2u);
  EXPECT_GT(info.duration, base::Seconds(2));
  EXPECT_LT(info.duration, base::Seconds(4));
  EXPECT_NE(demuxer_->GetStream(DemuxerStreamType::kVideo), nullptr);
  EXPECT_NE(demuxer_->GetStream(DemuxerStreamType::kAudio), nullptr);
  EXPECT_TRUE(host_.errors.empty());
  std::remove(dst.c_str());
}

TEST_F(RemuxerTest, UnwritableDestinationIsAnActionableError) {
  const Status status =
      RemuxContainer(TestFile("small_h264_aac_3s.mp4"),
                     "/nonexistent_dir_for_tests/remux.mkv");
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::kSourceReadFailed);
}

}  // namespace
}  // namespace avbase::media
