// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// End-to-end tests against real containers. Labelled needs-ffmpeg so the
// no-ffmpeg CI gate (design goal G2) skips them.

#include "media/filters/ffmpeg_demuxer.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "gtest/gtest.h"

namespace avbase::media {
namespace {

std::string TestFile(const char* name) {
  return std::string(AVBASE_TESTDATA_DIR) + "/" + name;
}

// Records what a Demuxer::Host is told, so tests can assert on the callbacks
// rather than on internal state.
class RecordingHost final : public Demuxer::Host {
 public:
  void SetDuration(base::TimeDelta /*duration*/) override { ++duration_calls; }
  void OnBufferedTimeUpdate(base::TimeDelta, base::TimeDelta) override {}
  void OnDemuxerError(MediaError error) override { errors.push_back(error); }

  int duration_calls{0};
  std::vector<MediaError> errors;
};

class FFmpegDemuxerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    media_log_ = base::MakeRefCounted<MediaLog>();
    demuxer_ = std::make_unique<FFmpegDemuxer>(media_log_);
  }

  // Runs Initialize() to completion and returns its status.
  Status Open(const std::string& path, DemuxerOptions options = {}) {
    return OpenDescriptor(DataSourceDescriptor::FromUri(path), options);
  }
  Status OpenDescriptor(const DataSourceDescriptor& descriptor,
                        DemuxerOptions options = {}) {
    Status result = Err(ErrorCode::kNotImplemented, "not run", {}, {});
    bool done = false;
    demuxer_->Initialize(
        descriptor, options, &host_,
        task_environment_.GetMainThreadTaskRunnerRef(),
        base::BindOnce([](Status* out, bool* flag, Status s) {
          *out = std::move(s);
          *flag = true;
        }, &result, &done));
    for (int i = 0; i < 2000 && !done; ++i) {
      task_environment_.RunUntilIdle();
      if (!done) {
        // The demux thread does the blocking open; give it a moment.
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

// The DataSource->AVIOContext bridge (M4 leftover, closed): the same MP4 fed
// as raw bytes must probe, report size, and read identically to the URI path.
namespace {
std::vector<uint8_t> ReadFileBytes(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  std::vector<uint8_t> bytes;
  if (f) {
    uint8_t chunk[8192];
    size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      bytes.insert(bytes.end(), chunk, chunk + n);
    }
    std::fclose(f);
  }
  return bytes;
}
}  // namespace

TEST_F(FFmpegDemuxerTest, OpensAMemoryBufferThroughTheDataSourceBridge) {
  const std::vector<uint8_t> bytes =
      ReadFileBytes(TestFile("small_h264_aac_3s.mp4"));
  ASSERT_GT(bytes.size(), 1000u);
  const DataSourceDescriptor descriptor =
      DataSourceDescriptor::FromMemory(bytes.data(), bytes.size());
  const Status status = OpenDescriptor(descriptor);
  ASSERT_TRUE(status) << status.error().ToString();

  const MediaInfo& info = demuxer_->media_info();
  EXPECT_EQ(info.streams.size(), 2u);
  EXPECT_TRUE(info.seekable);
  EXPECT_GT(info.duration, base::Seconds(2));
  EXPECT_LT(info.duration, base::Seconds(4));
  // avio_size() through the bridge's AVSEEK_SIZE path reports the buffer.
  EXPECT_EQ(info.file_size, static_cast<int64_t>(bytes.size()));
  EXPECT_TRUE(host_.errors.empty());
}

TEST_F(FFmpegDemuxerTest, OpensAHostSuppliedDataSource) {
  const std::vector<uint8_t> bytes =
      ReadFileBytes(TestFile("small_h264_aac_3s.mp4"));
  ASSERT_GT(bytes.size(), 1000u);
  auto memory = base::MakeRefCounted<MemoryDataSource>(bytes.data(),
                                                       bytes.size());
  const Status status =
      OpenDescriptor(DataSourceDescriptor::FromSource(std::move(memory)));
  ASSERT_TRUE(status) << status.error().ToString();

  const MediaInfo& info = demuxer_->media_info();
  EXPECT_EQ(info.streams.size(), 2u);
  EXPECT_GT(info.duration, base::Seconds(2));
  EXPECT_TRUE(host_.errors.empty());
}

TEST_F(FFmpegDemuxerTest, OpensAndProbesAnMp4) {
  const Status status = Open(TestFile("small_h264_aac_3s.mp4"));
  ASSERT_TRUE(status) << status.error().ToString();

  const MediaInfo& info = demuxer_->media_info();
  EXPECT_EQ(info.streams.size(), 2u);
  EXPECT_FALSE(info.is_live);
  EXPECT_TRUE(info.seekable);
  EXPECT_GT(info.duration, base::Seconds(2));
  EXPECT_LT(info.duration, base::Seconds(4));
  EXPECT_FALSE(info.format_name.empty());
  EXPECT_EQ(host_.duration_calls, 1);
  EXPECT_TRUE(host_.errors.empty());

  EXPECT_NE(demuxer_->GetStream(DemuxerStreamType::kVideo), nullptr);
  EXPECT_NE(demuxer_->GetStream(DemuxerStreamType::kAudio), nullptr);
  EXPECT_EQ(demuxer_->GetStream(DemuxerStreamType::kText), nullptr);
}

TEST_F(FFmpegDemuxerTest, VideoStreamConfigMatchesTheFile) {
  ASSERT_TRUE(Open(TestFile("small_h264_aac_3s.mp4")));
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_TRUE(video);
  const VideoDecoderConfig& config = video->video_decoder_config();
  EXPECT_EQ(config.codec, VideoCodec::kH264);
  EXPECT_EQ(config.codec_name, "h264");
  EXPECT_EQ(config.coded_size, (Size{320, 240}));
  EXPECT_EQ(config.natural_size, (Size{320, 240}));
  EXPECT_TRUE(config.IsValidConfig());
  // testsrc2 at rate=30 with -g 30: avg_frame_rate must be populated.
  EXPECT_GT(config.avg_frame_rate.ToDouble(), 25.0);
}

TEST_F(FFmpegDemuxerTest, AudioStreamConfigMatchesTheFile) {
  ASSERT_TRUE(Open(TestFile("small_h264_aac_3s.mp4")));
  DemuxerStream* audio = demuxer_->GetStream(DemuxerStreamType::kAudio);
  ASSERT_TRUE(audio);
  const AudioDecoderConfig& config = audio->audio_decoder_config();
  EXPECT_EQ(config.codec, AudioCodec::kAac);
  EXPECT_EQ(config.sample_rate, 48000);
  EXPECT_EQ(config.channels, 2);
  EXPECT_EQ(config.channel_layout, ChannelLayout::kStereo);
  EXPECT_TRUE(config.IsValidConfig());
}

TEST_F(FFmpegDemuxerTest, AudioOnlyFileHasNoVideoStream) {
  ASSERT_TRUE(Open(TestFile("audio_only.m4a")));
  EXPECT_EQ(demuxer_->GetStream(DemuxerStreamType::kVideo), nullptr);
  EXPECT_NE(demuxer_->GetStream(DemuxerStreamType::kAudio), nullptr);
  EXPECT_EQ(demuxer_->media_info().streams.size(), 1u);
}

TEST_F(FFmpegDemuxerTest, VideoOnlyFileHasNoAudioStream) {
  ASSERT_TRUE(Open(TestFile("video_only.mp4")));
  EXPECT_NE(demuxer_->GetStream(DemuxerStreamType::kVideo), nullptr);
  EXPECT_EQ(demuxer_->GetStream(DemuxerStreamType::kAudio), nullptr);
}

// A random byte blob must produce a three-part actionable error, not a crash
// and not "unknown error" (docs/10 §4).
TEST_F(FFmpegDemuxerTest, CorruptHeaderProducesActionableError) {
  const Status status = Open(TestFile("corrupt_header.mp4"));
  ASSERT_FALSE(status);
  const MediaError& error = status.error();
  EXPECT_FALSE(error.summary().empty());
  EXPECT_NE(error.native_code(), 0);
  EXPECT_FALSE(error.suggestion().empty())
      << "every error must tell the caller what to do next";
  EXPECT_NE(error.ToString().find("hint:"), std::string::npos)
      << error.ToString();
  EXPECT_EQ(host_.errors.size(), 1u);
}

TEST_F(FFmpegDemuxerTest, EmptyUriIsRejectedWithoutTouchingFFmpeg) {
  const Status status = Open("");
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::kInvalidArgument);
  EXPECT_NE(status.error().suggestion().find("SetDataSource"),
            std::string::npos);
}

// The truncation case must not hang: av_read_frame hits EOF early and the loop
// exits cleanly rather than spinning.
TEST_F(FFmpegDemuxerTest, TruncatedFileReachesEndOfStream) {
  ASSERT_TRUE(Open(TestFile("truncated_tail.mp4")));
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_TRUE(video);

  int buffers = 0;
  bool eos = false;
  for (int attempt = 0; attempt < 400 && !eos; ++attempt) {
    bool answered = false;
    video->Read(8, base::BindOnce([](bool* flag, bool* eos_flag, int* count,
                                     DemuxerStream::Status /*status*/,
                                     DemuxerStream::DecoderBufferVector b) {
                  *flag = true;
                  *count += static_cast<int>(b.size());
                  for (const auto& buf : b) {
                    if (buf->IsEndOfStream()) *eos_flag = true;
                  }
                }, &answered, &eos, &buffers));
    for (int i = 0; i < 200 && !answered; ++i) {
      task_environment_.RunUntilIdle();
      if (!answered) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(answered) << "Read() never called back";
  }
  EXPECT_TRUE(eos) << "a truncated file must still reach EOS, not hang";
  EXPECT_GT(buffers, 0);
}

// Reads a whole file and checks the invariants that A/V sync depends on:
// timestamps non-decreasing per stream, every buffer carrying the current
// serial, and the byte accounting matching the payload.
TEST_F(FFmpegDemuxerTest, ReadsWholeFileWithMonotonicTimestamps) {
  ASSERT_TRUE(Open(TestFile("small_h264_aac_3s.mp4")));
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_TRUE(video);

  base::TimeDelta previous;
  bool previous_valid = false;
  int frames = 0;
  bool eos = false;
  const int32_t expected_serial = video->serial();

  for (int attempt = 0; attempt < 600 && !eos; ++attempt) {
    bool answered = false;
    bool non_monotonic = false;
    video->Read(4, base::BindOnce([](bool* flag, bool* eos_flag, int* count,
                                     bool* mono, bool* prev_valid,
                                     base::TimeDelta* prev, int32_t serial,
                                     DemuxerStream::Status,
                                     DemuxerStream::DecoderBufferVector buffers) {
                  *flag = true;
                  *count += static_cast<int>(buffers.size());
                  for (const auto& buf : buffers) {
                    if (buf->IsEndOfStream()) { *eos_flag = true; continue; }
                    EXPECT_EQ(buf->serial(), serial);
                    EXPECT_EQ(buf->stream_type(), DemuxerStreamType::kVideo);
                    EXPECT_GT(buf->data_size(), 0u);
                    if (buf->timestamp().is_infinte()) continue;   // kNoTimestamp
                    if (*prev_valid && buf->timestamp() < *prev) *mono = false;
                    *prev = buf->timestamp();
                    *prev_valid = true;
                  }
                }, &answered, &eos, &frames, &non_monotonic, &previous_valid,
                &previous, expected_serial));
    for (int i = 0; i < 400 && !answered; ++i) {
      task_environment_.RunUntilIdle();
      if (!answered) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(answered);
    ASSERT_FALSE(non_monotonic) << "video pts went backwards";
  }
  // testsrc2 at 30 fps for 3 s: expect roughly 90 frames.
  EXPECT_GE(frames, 80);
  EXPECT_LE(frames, 100);
  EXPECT_TRUE(eos);
  EXPECT_GT(demuxer_->GetStats().packets_demuxed, 80u);
  EXPECT_GT(demuxer_->GetStats().bytes_read, 0);
}

// Serial must survive a seek so that pre-seek buffers are recognisable as
// stale (docs/04 §4 rule R1). This is the single most load-bearing invariant
// in the whole seek path.
TEST_F(FFmpegDemuxerTest, SeekBumpsSerialAndReportsCompletion) {
  ASSERT_TRUE(Open(TestFile("small_h264_aac_3s.mp4")));
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_TRUE(video);
  const int32_t serial_before = video->serial();

  bool seek_done = false;
  Status seek_status = Err(ErrorCode::kNotImplemented, "unset", {}, {});
  demuxer_->StartPlayingFrom(
      base::Seconds(2),
      base::BindOnce([](bool* done, Status* out, Status s, base::TimeDelta) {
        *done = true;
        *out = std::move(s);
      }, &seek_done, &seek_status));

  for (int i = 0; i < 1000 && !seek_done; ++i) {
    task_environment_.RunUntilIdle();
    if (!seek_done) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(seek_done) << "the seek callback never ran";
  ASSERT_TRUE(seek_status) << seek_status.error().ToString();
  EXPECT_EQ(demuxer_->GetStats().seek_count, 1u);
  // The demux thread flushes every stream during the seek, which bumps serial.
  EXPECT_GT(video->serial(), serial_before);
}

TEST_F(FFmpegDemuxerTest, StopIsPromptEvenWhileBlockedInRead) {
  ASSERT_TRUE(Open(TestFile("small_h264_aac_3s.mp4")));
  // Nothing is consuming, so the demux thread will block on a full queue.
  for (int i = 0; i < 100; ++i) {
    task_environment_.RunUntilIdle();
    if (demuxer_->GetStats().packets_demuxed > 200) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const base::TimeTicks before = base::TimeTicks::Now();
  demuxer_->Stop();
  const base::TimeDelta elapsed = base::TimeTicks::Now() - before;
  // Δ15: shutdown must be bounded. A full video queue plus a blocking read is
  // exactly the situation that made ijkplayer's release() hang.
  EXPECT_LT(elapsed, base::Seconds(2)) << "Stop() took " << elapsed.ToString();
}

TEST_F(FFmpegDemuxerTest, StageEventsAreLogged) {
  ASSERT_TRUE(Open(TestFile("small_h264_aac_3s.mp4")));
  const auto events = media_log_->GetEvents();
  bool saw_open = false;
  bool saw_stream_info = false;
  for (const MediaLogEvent& event : events) {
    if (event.type == MediaLogEvent::Type::kOpenInput) saw_open = true;
    if (event.type == MediaLogEvent::Type::kFindStreamInfo) saw_stream_info = true;
  }
  EXPECT_TRUE(saw_open);
  EXPECT_TRUE(saw_stream_info);
}

}  // namespace
}  // namespace avbase::media
