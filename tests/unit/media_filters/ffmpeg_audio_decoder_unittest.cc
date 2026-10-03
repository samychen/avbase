// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// End-to-end: real container -> FFmpegDemuxer -> FFmpegAudioDecoder -> real
// PCM. Mirrors ffmpeg_video_decoder_unittest.cc so the two decoder paths stay
// comparable.

#include "media/filters/ffmpeg_audio_decoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "gtest/gtest.h"
#include "media/base/audio_bus.h"
#include "media/filters/ffmpeg_demuxer.h"

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

// Pumps demux + audio decode together until the stream ends or |wanted| buffers
// arrive.
class AudioDecodePipeline {
 public:
  AudioDecodePipeline(base::test::TaskEnvironment& env)
      : env_(env),
        media_log_(base::MakeRefCounted<MediaLog>()),
        demuxer_(std::make_unique<FFmpegDemuxer>(media_log_)),
        decoder_(std::make_unique<FFmpegAudioDecoder>()) {}

  bool Open(const std::string& path) {
    Status result = Err(ErrorCode::kNotImplemented, "not run", {}, {});
    bool done = false;
    demuxer_->Initialize(DataSourceDescriptor::FromUri(path), DemuxerOptions{},
                         &host_, env_.GetMainThreadTaskRunnerRef(),
                         base::BindOnce(
                             [](Status* out, bool* flag, Status s) {
                               *out = std::move(s);
                               *flag = true;
                             },
                             &result, &done));
    PumpUntil([&done] { return done; });
    return done && result.has_value();
  }

  size_t Decode(size_t wanted, size_t max_rounds = 4000) {
    DemuxerStream* audio = demuxer_->GetStream(DemuxerStreamType::kAudio);
    if (!audio) {
      return 0;
    }
    config_ = audio->audio_decoder_config();

    bool init_done = false;
    decoder_->Initialize(
        config_, /*has_pending_clear=*/false, /*serial=*/0,
        base::BindOnce(
            [](bool* flag, DecoderStatus* out, DecoderStatus s) {
              *flag = true;
              *out = s;
            },
            &init_done, &init_status_),
        base::BindRepeating(
            [](std::vector<base::scoped_refptr<AudioBuffer>>* sink,
               base::scoped_refptr<AudioBuffer> buffer) {
              sink->push_back(std::move(buffer));
            },
            &buffers_),
        WaitingCB());
    PumpUntil([&init_done] { return init_done; });
    if (!init_status_.is_ok()) {
      return 0;
    }

    bool eos = false;
    for (size_t round = 0; round < max_rounds && CountReal() < wanted && !eos;
         ++round) {
      bool answered = false;
      audio->Read(8, base::BindOnce(
                         [](bool* flag, bool* eos_flag, DemuxerStream::Status,
                            DemuxerStream::DecoderBufferVector buffers) {
                           *flag = true;
                           for (const auto& b : buffers) {
                             if (b->IsEndOfStream()) {
                               *eos_flag = true;
                             }
                           }
                           PendingBuffers() = std::move(buffers);
                         },
                         &answered, &eos));
      PumpUntil([&answered] { return answered; });
      if (!answered) {
        break;
      }
      for (auto& buffer : PendingBuffers()) {
        bool decode_done = false;
        decoder_->Decode(
            buffer,
            base::BindOnce([](bool* flag, DecoderStatus) { *flag = true; },
                           &decode_done));
        PumpUntil([&decode_done] { return decode_done; });
      }
      PendingBuffers().clear();
    }

    // Drain so any frame the decoder was holding back (AAC priming) is emitted.
    bool flushed = false;
    decoder_->Decode(
        DecoderBuffer::CreateEOSBuffer(),
        base::BindOnce([](bool* flag, DecoderStatus) { *flag = true; },
                       &flushed));
    PumpUntil([&flushed] { return flushed; });
    return CountReal();
  }

  const AudioDecoderConfig& config() const { return config_; }
  const DecoderStatus& init_status() const { return init_status_; }
  const std::vector<base::scoped_refptr<AudioBuffer>>& buffers() const {
    return buffers_;
  }
  const std::vector<MediaError>& errors() const { return host_.errors; }

  // Buffers that actually carry samples (excludes the terminal EOS marker).
  size_t CountReal() const {
    size_t n = 0;
    for (const auto& b : buffers_) {
      if (!b->end_of_stream()) {
        ++n;
      }
    }
    return n;
  }

  int64_t TotalFrames() const {
    int64_t total = 0;
    for (const auto& b : buffers_) {
      if (!b->end_of_stream()) {
        total += b->frame_count();
      }
    }
    return total;
  }

 private:
  static DemuxerStream::DecoderBufferVector& PendingBuffers() {
    static DemuxerStream::DecoderBufferVector* buffers =
        new DemuxerStream::DecoderBufferVector();
    return *buffers;
  }

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
  std::unique_ptr<FFmpegAudioDecoder> decoder_;
  SilentHost host_;
  AudioDecoderConfig config_;
  DecoderStatus init_status_;
  std::vector<base::scoped_refptr<AudioBuffer>> buffers_;
};

class FFmpegAudioDecoderTest : public ::testing::Test {
 protected:
  base::test::TaskEnvironment task_environment_;
};

TEST_F(FFmpegAudioDecoderTest, DecodesEveryBufferOfARealFile) {
  AudioDecodePipeline pipeline(task_environment_);
  ASSERT_TRUE(pipeline.Open(TestFile("small_h264_aac_3s.mp4")))
      << "demuxer failed to open the test file";

  const size_t decoded = pipeline.Decode(/*wanted=*/100000);
  ASSERT_TRUE(pipeline.init_status().is_ok())
      << pipeline.init_status().AsDebugString();
  EXPECT_GT(decoded, 50u) << "decoded only " << decoded << " audio buffers";
  EXPECT_TRUE(pipeline.errors().empty());
}

TEST_F(FFmpegAudioDecoderTest, ProducesCorrectSampleRateAndChannels) {
  AudioDecodePipeline pipeline(task_environment_);
  ASSERT_TRUE(pipeline.Open(TestFile("audio_only.m4a")));
  ASSERT_GT(pipeline.Decode(10), 0u);

  const AudioDecoderConfig& config = pipeline.config();
  EXPECT_GT(config.sample_rate, 0);
  for (const auto& b : pipeline.buffers()) {
    if (b->end_of_stream()) {
      continue;
    }
    EXPECT_EQ(config.sample_rate, b->sample_rate());
    EXPECT_GT(b->channel_count(), 0);
    EXPECT_GT(b->frame_count(), 0)
        << "a non-EOS buffer with zero frames would stall the renderer";
    EXPECT_FALSE(b->data().empty());
  }
}

TEST_F(FFmpegAudioDecoderTest, TimestampsAreMonotonicAndCoverTheDuration) {
  AudioDecodePipeline pipeline(task_environment_);
  ASSERT_TRUE(pipeline.Open(TestFile("small_h264_aac_3s.mp4")));
  ASSERT_GT(pipeline.Decode(100000), 50u);

  base::TimeDelta previous;
  bool first = true;
  int64_t total_frames = 0;
  for (const auto& b : pipeline.buffers()) {
    if (b->end_of_stream()) {
      continue;
    }
    total_frames += b->frame_count();
    if (b->timestamp() != media::kNoTimestamp) {
      // Non-decreasing: AAC has no B-frames, so audio pts must never go
      // backwards. A regression here means pkt_timebase was not propagated
      // (bug #26 on the video side).
      if (!first) {
        EXPECT_GE(b->timestamp(), previous)
            << "pts went backwards at buffer " << b->AsDebugString();
      }
      previous = b->timestamp();
      first = false;
    }
  }
  ASSERT_FALSE(first) << "every buffer had a null timestamp";
  // 3 s at the stream sample rate. Allow wide tolerance for encoder priming and
  // the last partial buffer.
  const int rate = pipeline.config().sample_rate;
  const double seconds = static_cast<double>(total_frames) / rate;
  EXPECT_GT(seconds, 2.0) << "only " << seconds << " s of audio decoded";
  EXPECT_LT(seconds, 4.5) << seconds << " s decoded from a 3 s file";
}

TEST_F(FFmpegAudioDecoderTest, DecodedSamplesConvertToFloatWithoutClipping) {
  AudioDecodePipeline pipeline(task_environment_);
  ASSERT_TRUE(pipeline.Open(TestFile("audio_only.m4a")));
  ASSERT_GT(pipeline.Decode(10), 0u);

  // The point of AudioBuffer::ReadFrames: a real decoder's output must land in
  // [-1, 1] when read into an AudioBus, for whatever native format AAC used.
  int checked = 0;
  float peak = 0.0f;
  for (const auto& b : pipeline.buffers()) {
    if (b->end_of_stream() || checked >= 3) {
      continue;
    }
    auto bus = AudioBus::Create(b->channel_count(), b->frame_count());
    ASSERT_EQ(b->frame_count(), b->ReadFrames(b->frame_count(), 0, bus.get()));
    for (int ch = 0; ch < bus->channels(); ++ch) {
      for (int f = 0; f < bus->frames(); ++f) {
        const float v = bus->channel(ch)[f];
        ASSERT_FALSE(std::isnan(v)) << "NaN sample in " << b->AsDebugString();
        peak = std::max(peak, std::abs(v));
      }
    }
    ++checked;
  }
  EXPECT_GT(checked, 0);
  EXPECT_LE(peak, 1.0f + 1e-5f) << "peak " << peak << " exceeds full scale";
}

TEST_F(FFmpegAudioDecoderTest, EndOfStreamMarkerIsEmittedExactlyOnce) {
  AudioDecodePipeline pipeline(task_environment_);
  ASSERT_TRUE(pipeline.Open(TestFile("audio_only.m4a")));
  ASSERT_GT(pipeline.Decode(10), 0u);

  int eos_count = 0;
  for (const auto& b : pipeline.buffers()) {
    if (b->end_of_stream()) {
      ++eos_count;
    }
  }
  EXPECT_EQ(1, eos_count) << "the renderer needs exactly one EOS to shut down";
}

TEST_F(FFmpegAudioDecoderTest, DecodeBeforeInitializeFailsCleanly) {
  FFmpegAudioDecoder decoder;
  DecoderStatus status;
  bool ran = false;
  // Decode() on an uninitialized decoder must report, not crash and not emit.
  decoder.Decode(DecoderBuffer::CreateEOSBuffer(),
                 base::BindOnce(
                     [](bool* flag, DecoderStatus* out, DecoderStatus s) {
                       *flag = true;
                       *out = s;
                     },
                     &ran, &status));
  EXPECT_TRUE(ran);
  EXPECT_FALSE(status.is_ok());
  EXPECT_EQ(DecoderStatus::Codes::kNotInitialized, status.code());
  // Reset() before Initialize() must also be safe.
  bool reset_ran = false;
  decoder.Reset(base::BindOnce([](bool* flag) { *flag = true; }, &reset_ran));
  EXPECT_TRUE(reset_ran);
}

TEST_F(FFmpegAudioDecoderTest, UnsupportedCodecReportsActionableError) {
  FFmpegAudioDecoder decoder;
  // A structurally valid config whose codec_name libavcodec does not know: the
  // realistic case is a container advertising a codec this FFmpeg build lacks.
  AudioDecoderConfig config;
  config.codec_name = "definitely-not-a-codec";
  config.codec = AudioCodec::kAac;  // valid enum, bogus name
  config.sample_rate = 44100;
  config.channels = 2;

  DecoderStatus status;
  bool done = false;
  decoder.Initialize(config, false, 0,
                     base::BindOnce(
                         [](bool* flag, DecoderStatus* out, DecoderStatus s) {
                           *flag = true;
                           *out = s;
                         },
                         &done, &status),
                     FFmpegAudioDecoder::OutputCB(), WaitingCB());
  ASSERT_TRUE(done);
  EXPECT_FALSE(status.is_ok());
  EXPECT_EQ(DecoderStatus::Codes::kUnsupportedCodec, status.code());
  // The description must name the codec, otherwise the SDK user cannot tell
  // which track failed (see bug #28).
  EXPECT_NE(std::string::npos,
            status.description().find("definitely-not-a-codec"))
      << status.description();
  EXPECT_FALSE(decoder.initialized());
}

TEST_F(FFmpegAudioDecoderTest,
       StructurallyInvalidConfigIsDistinctFromUnknownCodec) {
  // Bug #28 on the video side was exactly this: one generic "config is not
  // valid" message for two different failures. The two codes must stay distinct
  // so an SDK user can tell "your file is malformed" from "we lack this codec".
  FFmpegAudioDecoder decoder;

  AudioDecoderConfig bad;
  bad.codec_name = "aac";
  bad.codec = AudioCodec::kUnknown;  // <- the invalid part
  bad.sample_rate = 44100;
  bad.channels = 2;

  DecoderStatus status;
  bool done = false;
  decoder.Initialize(bad, false, 0,
                     base::BindOnce(
                         [](bool* flag, DecoderStatus* out, DecoderStatus s) {
                           *flag = true;
                           *out = s;
                         },
                         &done, &status),
                     FFmpegAudioDecoder::OutputCB(), WaitingCB());
  ASSERT_TRUE(done);
  EXPECT_EQ(DecoderStatus::Codes::kUnsupportedConfig, status.code());
  EXPECT_FALSE(decoder.initialized());

  // A zero sample rate is invalid for the same reason: buffer durations would
  // divide by zero.
  AudioDecoderConfig no_rate;
  no_rate.codec_name = "aac";
  no_rate.codec = AudioCodec::kAac;
  no_rate.sample_rate = 0;
  no_rate.channels = 2;
  done = false;
  decoder.Initialize(no_rate, false, 0,
                     base::BindOnce(
                         [](bool* flag, DecoderStatus* out, DecoderStatus s) {
                           *flag = true;
                           *out = s;
                         },
                         &done, &status),
                     FFmpegAudioDecoder::OutputCB(), WaitingCB());
  ASSERT_TRUE(done);
  EXPECT_EQ(DecoderStatus::Codes::kUnsupportedConfig, status.code());
}

}  // namespace
}  // namespace avbase::media
