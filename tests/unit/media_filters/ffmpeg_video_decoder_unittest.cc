// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// End-to-end: real container -> FFmpegDemuxer -> FFmpegVideoDecoder -> real
// pixel data. This is the first test in the project that proves the whole
// demux/decode chain works, not just its parts.

#include "media/filters/ffmpeg_video_decoder.h"

#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "media/base/video_decoder_factory.h"
#include "media/filters/decoder_selector.h"
#include "media/filters/ffmpeg_demuxer.h"
#include "gtest/gtest.h"

namespace avbase::media {
namespace {

std::string TestFile(const char* name) {
  return std::string(AVBASE_TESTDATA_DIR) + "/" + name;
}

class SilentHost final : public Demuxer::Host {
 public:
  void SetDuration(base::TimeDelta) override {}
  void OnBufferedTimeUpdate(base::TimeDelta, base::TimeDelta) override {}
  void OnDemuxerError(MediaError error) override { errors.push_back(error); }
  std::vector<MediaError> errors;
};

// Pumps the demuxer and decoder together until |wanted| frames arrive or the
// stream ends. Returns the decoded frames.
class DecodePipeline {
 public:
  DecodePipeline(base::test::TaskEnvironment& env, std::string path)
      : env_(env),
        media_log_(base::MakeRefCounted<MediaLog>()),
        demuxer_(std::make_unique<FFmpegDemuxer>(media_log_)),
        decoder_(std::make_unique<FFmpegVideoDecoder>(
            env.GetMainThreadTaskRunnerRef())),
        path_(std::move(path)) {}

  bool Open() {
    Status result = Err(ErrorCode::kNotImplemented, "not run", {}, {});
    bool done = false;
    demuxer_->Initialize(
        DataSourceDescriptor::FromUri(path_), DemuxerOptions{}, &host_,
        env_.GetMainThreadTaskRunnerRef(),
        base::BindOnce([](Status* out, bool* flag, Status s) {
          *out = std::move(s);
          *flag = true;
        }, &result, &done));
    PumpUntil([&done] { return done; });
    return done && result.has_value();
  }
  // Runs the demux->decode loop until |wanted| frames are produced or EOS.
  size_t Decode(size_t wanted, size_t max_rounds = 4000) {
    DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
    if (!video) {
      return 0;
    }
    const VideoDecoderConfig config = video->video_decoder_config();

    bool init_done = false;
    DecoderStatus init_status;
    decoder_->Initialize(
        config, /*low_delay=*/false, /*cdm=*/nullptr,
        base::BindOnce([](bool* flag, DecoderStatus* out, DecoderStatus s) {
          *flag = true;
          *out = s;
        }, &init_done, &init_status),
        base::BindRepeating(
            [](std::vector<base::scoped_refptr<VideoFrame>>* sink,
               base::scoped_refptr<VideoFrame> frame) {
              sink->push_back(std::move(frame));
            },
            &frames_),
        WaitingCB());
    PumpUntil([&init_done] { return init_done; });
    if (!init_status.is_ok()) {
      return 0;
    }

    bool eos = false;
    for (size_t round = 0; round < max_rounds && frames_.size() < wanted && !eos;
         ++round) {
      bool answered = false;
      video->Read(4, base::BindOnce([](bool* flag, bool* eos_flag,
                                       DemuxerStream::Status,
                                       DemuxerStream::DecoderBufferVector buffers) {
                    *flag = true;
                    for (const auto& b : buffers) {
                      if (b->IsEndOfStream()) *eos_flag = true;
                    }
                    // Store for the decode step below.
                    PendingBuffers() = std::move(buffers);
                  }, &answered, &eos));
      PumpUntil([&answered] { return answered; });
      if (!answered) {
        break;
      }
      for (auto& buffer : PendingBuffers()) {
        bool decode_done = false;
        decoder_->Decode(buffer, base::BindOnce([](bool* flag, DecoderStatus) {
                                           *flag = true;
                                         }, &decode_done));
        PumpUntil([&decode_done] { return decode_done; });
      }
      PendingBuffers().clear();
    }

    // Flush the decoder so buffered frames (B-frame reorder delay) come out.
    bool flushed = false;
    decoder_->Decode(DecoderBuffer::CreateEOSBuffer(),
                     base::BindOnce([](bool* flag, DecoderStatus) { *flag = true; },
                                    &flushed));
    PumpUntil([&flushed] { return flushed; });
    return frames_.size();
  }

  const std::vector<base::scoped_refptr<VideoFrame>>& frames() const {
    return frames_;
  }
  const std::vector<MediaError>& errors() const { return host_.errors; }

 private:
  static DemuxerStream::DecoderBufferVector& PendingBuffers() {
    static DemuxerStream::DecoderBufferVector* buffers =
        new DemuxerStream::DecoderBufferVector();
    return *buffers;
  }

  // Runs the test-sequence queue, yielding briefly so the demux thread can make
  // progress. Bounded so a hang becomes a test failure rather than a timeout.
  void PumpUntil(const std::function<bool()>& predicate, int max_spins = 4000) {
    for (int i = 0; i < max_spins && !predicate(); ++i) {
      env_.RunUntilIdle();
      if (!predicate()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }

  base::test::TaskEnvironment& env_;
  base::scoped_refptr<MediaLog> media_log_;
  std::unique_ptr<FFmpegDemuxer> demuxer_;
  std::unique_ptr<FFmpegVideoDecoder> decoder_;
  SilentHost host_;
  std::string path_;
  std::vector<base::scoped_refptr<VideoFrame>> frames_;
};

class FFmpegVideoDecoderTest : public ::testing::Test {
 protected:
  base::test::TaskEnvironment task_environment_;
};

TEST_F(FFmpegVideoDecoderTest, DecodesEveryFrameOfARealFile) {
  DecodePipeline pipeline(task_environment_, TestFile("small_h264_aac_3s.mp4"));
  ASSERT_TRUE(pipeline.Open()) << "demuxer failed to open the test file";

  // testsrc2 at 30 fps for 3 s: 90 frames. The decoder must produce essentially
  // all of them; a few may be lost to the reorder buffer at EOS.
  const size_t decoded = pipeline.Decode(/*wanted=*/200);
  EXPECT_GE(decoded, 85u) << "decoded only " << decoded << " frames";
  EXPECT_LE(decoded, 95u);
  EXPECT_TRUE(pipeline.errors().empty());
}

TEST_F(FFmpegVideoDecoderTest, DecodedFramesCarryCorrectGeometry) {
  DecodePipeline pipeline(task_environment_, TestFile("small_h264_aac_3s.mp4"));
  ASSERT_TRUE(pipeline.Open());
  ASSERT_GE(pipeline.Decode(/*wanted=*/5), 5u);

  const auto& frames = pipeline.frames();
  ASSERT_FALSE(frames.empty());
  const VideoFrame& first = *frames.front();
  EXPECT_EQ(first.coded_size(), (Size{320, 240}));
  EXPECT_EQ(first.format(), VideoFormat::kI420);
  EXPECT_EQ(first.storage_type(), VideoFrame::StorageType::kStorageOwned);
  EXPECT_TRUE(first.IsMappable());
  // The Y plane must actually contain pixel data, not a zero-filled allocation.
  const std::span<const uint8_t> y = first.visible_data(VideoFrame::kYPlane);
  ASSERT_GE(y.size(), 320u * 240u);
  bool has_non_black = false;
  for (size_t i = 0; i < y.size() && !has_non_black; i += 97) {
    has_non_black = (y[i] != 0);
  }
  EXPECT_TRUE(has_non_black) << "decoded luma is all zero — conversion produced "
                                "no pixels";
}

TEST_F(FFmpegVideoDecoderTest, TimestampsAdvanceAcrossDecodedFrames) {
  DecodePipeline pipeline(task_environment_, TestFile("small_h264_aac_3s.mp4"));
  ASSERT_TRUE(pipeline.Open());
  ASSERT_GE(pipeline.Decode(/*wanted=*/200), 20u);

  const auto& frames = pipeline.frames();
  base::TimeDelta previous;
  bool previous_valid = false;
  int regressions = 0;
  for (const auto& frame : frames) {
    if (frame->timestamp().is_infinte()) {
      continue;
    }
    if (previous_valid && frame->timestamp() < previous) {
      ++regressions;
    }
    previous = frame->timestamp();
    previous_valid = true;
  }
  // Frames come out of the decoder in presentation order, so pts must not go
  // backwards. This is the invariant VideoFrameCompositor relies on when it
  // derives last_duration from a pts delta.
  EXPECT_EQ(regressions, 0) << "presentation timestamps went backwards "
                            << regressions << " time(s)";
  EXPECT_GT(previous, base::Seconds(2))
      << "last decoded pts should be near the 3 s mark";
}

TEST_F(FFmpegVideoDecoderTest, SerialPropagatesFromBufferToFrame) {
  DecodePipeline pipeline(task_environment_, TestFile("small_h264_aac_3s.mp4"));
  ASSERT_TRUE(pipeline.Open());
  ASSERT_GE(pipeline.Decode(/*wanted=*/10), 5u);
  for (const auto& frame : pipeline.frames()) {
    // No seek happened, so every frame carries the initial generation. Seeking
    // and re-checking this is covered by the demuxer's serial tests; what
    // matters here is that the decoder propagates it at all rather than
    // dropping it, which would silently break stale-frame detection.
    EXPECT_EQ(frame->serial(), 0);
  }
}

TEST_F(FFmpegVideoDecoderTest, UnsupportedCodecReportsActionableStatus) {
  auto decoder = std::make_unique<FFmpegVideoDecoder>(
      task_environment_.GetMainThreadTaskRunnerRef());
  VideoDecoderConfig config;
  config.codec = VideoCodec::kUnknown;
  config.codec_name = "definitely-not-a-real-codec";
  config.coded_size = Size{64, 64};

  bool done = false;
  DecoderStatus status;
  decoder->Initialize(config, false, nullptr,
                      base::BindOnce([](bool* flag, DecoderStatus* out,
                                        DecoderStatus s) {
                        *flag = true;
                        *out = s;
                      }, &done, &status),
                      VideoDecoder::OutputCB(), WaitingCB());
  for (int i = 0; i < 500 && !done; ++i) {
    task_environment_.RunUntilIdle();
    if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(done);
  EXPECT_FALSE(status.is_ok());
  EXPECT_EQ(status.code(), DecoderStatus::Codes::kUnsupportedCodec);
  EXPECT_NE(status.description().find("definitely-not-a-real-codec"),
            std::string::npos);
}

TEST_F(FFmpegVideoDecoderTest, DecodeBeforeInitializeReportsNotInitialized) {
  auto decoder = std::make_unique<FFmpegVideoDecoder>(
      task_environment_.GetMainThreadTaskRunnerRef());
  bool done = false;
  DecoderStatus status;
  decoder->Decode(DecoderBuffer::CreateEOSBuffer(),
                  base::BindOnce([](bool* flag, DecoderStatus* out,
                                    DecoderStatus s) {
                    *flag = true;
                    *out = s;
                  }, &done, &status));
  for (int i = 0; i < 500 && !done; ++i) {
    task_environment_.RunUntilIdle();
  }
  ASSERT_TRUE(done);
  EXPECT_EQ(status.code(), DecoderStatus::Codes::kNotInitialized);
}

// The contract that DecoderStream depends on: decode_cb must never run inside
// Decode(), otherwise a decoder that completes synchronously would re-enter the
// pump and blow the stack on a long stream.
TEST_F(FFmpegVideoDecoderTest, DecodeCallbackIsNeverRunInline) {
  auto decoder = std::make_unique<FFmpegVideoDecoder>(
      task_environment_.GetMainThreadTaskRunnerRef());
  VideoDecoderConfig config;
  config.codec = VideoCodec::kH264;
  config.codec_name = "h264";
  config.coded_size = Size{64, 64};

  bool init_done = false;
  decoder->Initialize(config, false, nullptr,
                      base::BindOnce([](bool* f, DecoderStatus) { *f = true; },
                                     &init_done),
                      VideoDecoder::OutputCB(), WaitingCB());
  for (int i = 0; i < 500 && !init_done; ++i) {
    task_environment_.RunUntilIdle();
  }

  bool ran_inline = false;
  bool cb_ran = false;
  decoder->Decode(DecoderBuffer::CreateEOSBuffer(),
                  base::BindOnce([](bool* flag, DecoderStatus) { *flag = true; },
                                 &cb_ran));
  ran_inline = cb_ran;   // True only if the callback fired before Decode returned.
  EXPECT_FALSE(ran_inline) << "decode_cb ran inline, violating the contract";

  for (int i = 0; i < 500 && !cb_ran; ++i) {
    task_environment_.RunUntilIdle();
  }
  EXPECT_TRUE(cb_ran);
}

// ---- DecoderSelector: pure ranking, exhaustively testable -------------------

class FakeVideoDecoderFactory final : public VideoDecoderFactory {
 public:
  FakeVideoDecoderFactory(std::string name, bool hardware, int priority = 0,
                          int max_width = 0)
      : name_(std::move(name)) {
    capability_.hardware = hardware;
    capability_.priority = priority;
    capability_.max_width = max_width;
  }
  VideoDecoderCapability GetCapability() const override { return capability_; }
  bool SupportsCodec(VideoDecoderType) const override { return true; }
  std::unique_ptr<VideoDecoder> CreateVideoDecoder(const VideoDecoderConfig&) override {
    return nullptr;
  }
  const char* name() const override { return name_.c_str(); }

 private:
  std::string name_;
  VideoDecoderCapability capability_;
};

VideoDecoderConfig H264Config(int width = 1920, int height = 1080) {
  VideoDecoderConfig config;
  config.codec = VideoCodec::kH264;
  config.codec_name = "h264";
  config.coded_size = Size{width, height};
  config.natural_size = config.coded_size;
  return config;
}

using FactoryList = std::vector<base::scoped_refptr<VideoDecoderFactory>>;

FactoryList MakeFactories() {
  return {base::MakeRefCounted<FakeVideoDecoderFactory>("vaapi", true, 10),
          base::MakeRefCounted<FakeVideoDecoderFactory>("ffmpeg", false, 0)};
}

TEST(DecoderSelectorTest, AutoPrefersHardwareThenSoftware) {
  const auto factories = MakeFactories();
  std::vector<std::string> reasons;
  const auto selected = DecoderSelector::SelectVideoDecoder(
      factories, H264Config(), DecoderPreference::kAuto,
      static_cast<HwCodecMask>(HwCodecFlag::kAll), &reasons);
  ASSERT_EQ(selected.size(), 2u);
  EXPECT_STREQ(selected[0]->name(), "vaapi");
  EXPECT_STREQ(selected[1]->name(), "ffmpeg");
}

TEST(DecoderSelectorTest, SoftwareExcludesHardware) {
  const auto factories = MakeFactories();
  const auto selected = DecoderSelector::SelectVideoDecoder(
      factories, H264Config(), DecoderPreference::kSoftware,
      static_cast<HwCodecMask>(HwCodecFlag::kAll), nullptr);
  ASSERT_EQ(selected.size(), 1u);
  EXPECT_STREQ(selected[0]->name(), "ffmpeg");
}

TEST(DecoderSelectorTest, HardwareOnlyDoesNotFallBack) {
  const auto factories = MakeFactories();
  const auto selected = DecoderSelector::SelectVideoDecoder(
      factories, H264Config(), DecoderPreference::kHardwareOnly,
      static_cast<HwCodecMask>(HwCodecFlag::kAll), nullptr);
  ASSERT_EQ(selected.size(), 1u);
  EXPECT_STREQ(selected[0]->name(), "vaapi");
}

// Replaces ijkplayer's mediacodec-avc / -hevc / -mpeg2 / -mpeg4 booleans with
// one mask, and the rejection must be explainable.
TEST(DecoderSelectorTest, HwCodecMaskFiltersByCodec) {
  const auto factories = MakeFactories();
  std::vector<std::string> reasons;
  const auto selected = DecoderSelector::SelectVideoDecoder(
      factories, H264Config(), DecoderPreference::kAuto,
      static_cast<HwCodecMask>(HwCodecFlag::kHevc), &reasons);
  ASSERT_EQ(selected.size(), 1u);
  EXPECT_STREQ(selected[0]->name(), "ffmpeg");
  bool explained = false;
  for (const std::string& reason : reasons) {
    if (reason.find("hw_codecs") != std::string::npos) explained = true;
  }
  EXPECT_TRUE(explained) << "rejection was not explained: "
                         << (reasons.empty() ? "(none)" : reasons[0]);
}

TEST(DecoderSelectorTest, ResolutionCapExcludesOversizedStreams) {
  FactoryList factories = {
      base::MakeRefCounted<FakeVideoDecoderFactory>("vaapi-1080p", true, 10, 1920),
      base::MakeRefCounted<FakeVideoDecoderFactory>("ffmpeg", false, 0)};
  std::vector<std::string> reasons;
  const auto selected = DecoderSelector::SelectVideoDecoder(
      factories, H264Config(3840, 2160), DecoderPreference::kAuto,
      static_cast<HwCodecMask>(HwCodecFlag::kAll), &reasons);
  ASSERT_EQ(selected.size(), 1u);
  EXPECT_STREQ(selected[0]->name(), "ffmpeg");
  bool explained = false;
  for (const std::string& reason : reasons) {
    if (reason.find("exceeds the decoder maximum") != std::string::npos) {
      explained = true;
    }
  }
  EXPECT_TRUE(explained);
}

TEST(DecoderSelectorTest, PriorityOrdersSameKindFactories) {
  FactoryList factories = {
      base::MakeRefCounted<FakeVideoDecoderFactory>("generic-hw", true, 1),
      base::MakeRefCounted<FakeVideoDecoderFactory>("vendor-hw", true, 100),
      base::MakeRefCounted<FakeVideoDecoderFactory>("ffmpeg", false, 0)};
  const auto selected = DecoderSelector::SelectVideoDecoder(
      factories, H264Config(), DecoderPreference::kAuto,
      static_cast<HwCodecMask>(HwCodecFlag::kAll), nullptr);
  ASSERT_EQ(selected.size(), 3u);
  EXPECT_STREQ(selected[0]->name(), "vendor-hw");
  EXPECT_STREQ(selected[1]->name(), "generic-hw");
  EXPECT_STREQ(selected[2]->name(), "ffmpeg");
}

TEST(DecoderSelectorTest, EmptyFactoryListExplainsItself) {
  std::vector<std::string> reasons;
  const auto selected = DecoderSelector::SelectVideoDecoder(
      {}, H264Config(), DecoderPreference::kAuto,
      static_cast<HwCodecMask>(HwCodecFlag::kAll), &reasons);
  EXPECT_TRUE(selected.empty());
  ASSERT_FALSE(reasons.empty());
  EXPECT_NE(reasons.back().find("no decoder factory can handle"),
            std::string::npos);
}

TEST(DecoderSelectorTest, CodecMaskHelpers) {
  const auto all = static_cast<HwCodecMask>(HwCodecFlag::kAll);
  EXPECT_TRUE(DecoderSelector::CodecAllowedByMask(VideoCodec::kH264, all));
  EXPECT_TRUE(DecoderSelector::CodecAllowedByMask(VideoCodec::kAv1, all));
  EXPECT_TRUE(DecoderSelector::CodecAllowedByMask(
      VideoCodec::kHevc, static_cast<HwCodecMask>(HwCodecFlag::kHevc)));
  EXPECT_FALSE(DecoderSelector::CodecAllowedByMask(
      VideoCodec::kHevc, static_cast<HwCodecMask>(HwCodecFlag::kAvc)));
  EXPECT_FALSE(DecoderSelector::CodecAllowedByMask(VideoCodec::kH264, 0));
  EXPECT_FALSE(DecoderSelector::CodecAllowedByMask(VideoCodec::kUnknown, all));
}

}  // namespace
}  // namespace avbase::media
