// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// HLS through the regular FFmpegDemuxer: FFmpeg's hls demuxer (enabled in
// tools/setup_ffmpeg.sh) sits under the same avio stack as every other
// container, so avbase needs no HLS-specific code -- these tests are the
// proof, using a two-segment VOD playlist over the file protocol (segments
// resolve relative to the playlist, no network involved).

#include "media/filters/ffmpeg_demuxer.h"

#include <chrono>
#include <string>
#include <thread>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "gtest/gtest.h"

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

class HlsDemuxerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    media_log_ = base::MakeRefCounted<MediaLog>();
    demuxer_ = std::make_unique<FFmpegDemuxer>(media_log_);
  }

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
    EXPECT_TRUE(done) << "Initialize() never reported back";
    return result;
  }

  base::test::TaskEnvironment task_environment_;
  base::scoped_refptr<MediaLog> media_log_;
  std::unique_ptr<FFmpegDemuxer> demuxer_;
  RecordingHost host_;
};

TEST_F(HlsDemuxerTest, OpensPlaylistAndProbesStreams) {
  ASSERT_TRUE(Open(TestFile("hls/local.m3u8")));

  const MediaInfo& info = demuxer_->media_info();
  EXPECT_EQ(info.streams.size(), 2u);
  EXPECT_FALSE(info.is_live) << "ENDLIST playlist is VOD, not live";
  EXPECT_GT(info.duration, base::Milliseconds(1500));
  EXPECT_LT(info.duration, base::Seconds(4));
  EXPECT_NE(demuxer_->GetStream(DemuxerStreamType::kVideo), nullptr);
  EXPECT_NE(demuxer_->GetStream(DemuxerStreamType::kAudio), nullptr);
}

TEST_F(HlsDemuxerTest, VideoStreamConfigMatchesTheFixture) {
  ASSERT_TRUE(Open(TestFile("hls/local.m3u8")));
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_TRUE(video);
  const VideoDecoderConfig& config = video->video_decoder_config();
  EXPECT_EQ(config.codec, VideoCodec::kH264);
  EXPECT_EQ(config.coded_size, (Size{128, 96}));
}

// The point of HLS is continuity across segment boundaries: reading past the
// first segment's duration must keep yielding buffers from seg1, not stall.
TEST_F(HlsDemuxerTest, ReadsAcrossSegmentsToEos) {
  ASSERT_TRUE(Open(TestFile("hls/local.m3u8")));
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_TRUE(video);

  int buffers = 0;
  bool eos = false;
  base::TimeDelta last_pts;
  for (int attempt = 0; attempt < 400 && !eos; ++attempt) {
    bool answered = false;
    video->Read(8, base::BindOnce(
                       [](bool* flag, bool* eos_flag, int* count,
                          base::TimeDelta* last, DemuxerStream::Status status,
                          DemuxerStream::DecoderBufferVector b) {
                         *flag = true;
                         ASSERT_EQ(status, DemuxerStream::Status::kOk);
                         *count += static_cast<int>(b.size());
                         for (const auto& buf : b) {
                           if (buf->IsEndOfStream()) {
                             *eos_flag = true;
                           } else if (*last < buf->timestamp()) {
                             *last = buf->timestamp();
                           }
                         }
                       },
                       &answered, &eos, &buffers, &last_pts));
    for (int i = 0; i < 200 && !answered; ++i) {
      task_environment_.RunUntilIdle();
      if (!answered) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    ASSERT_TRUE(answered) << "Read() never called back";
  }
  EXPECT_TRUE(eos);
  // Two 1-second segments at 10 fps: reading across the boundary must reach
  // the second half of the presentation.
  EXPECT_GT(last_pts, base::Seconds(1));
  EXPECT_GT(buffers, 10);
}

}  // namespace
}  // namespace avbase::media
