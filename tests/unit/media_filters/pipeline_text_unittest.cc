// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/threading/thread.h"
#include "base/time/default_tick_clock.h"
#include "gtest/gtest.h"
#include "media/base/audio_bus.h"
#include "media/base/data_source.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/pipeline_status.h"
#include "media/base/timed_text.h"
#include "media/filters/ffmpeg_decoder_factories.h"
#include "media/filters/ffmpeg_demuxer.h"
#include "media/filters/ffmpeg_text_decoder.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/pipeline_impl.h"
#include "media/renderers/default_renderer_factory.h"
#include "tests/support/fake_pipeline_client.h"
#include "tests/support/fake_renderer_sinks.h"
#include "tests/support/fake_sink_factories.h"

namespace avbase::media {
namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(45);
constexpr int kFramesPerBuffer = 256;
constexpr int kAudioChannels = 2;

std::vector<uint8_t> ReadFileBytes(const char* name) {
  const std::string path = std::string(AVBASE_TESTDATA_DIR) + "/" + name;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  std::vector<uint8_t> bytes;
  if (f) {
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      bytes.insert(bytes.end(), buf, buf + n);
    }
    std::fclose(f);
  }
  return bytes;
}

// Phase 4.2 text leg: the base renders nothing -- subtitle packets are
// decoded on S1 and delivered as TimedTextCues through Client::OnTimedText.
// The media is an audio file with TWO subrip tracks
// (tests/testdata/audio_two_subs.mkv, streams 1=eng and 2=chi), so selection
// has a real alternate and the delivery order is deterministic.
class PipelineTextTest : public ::testing::Test {
 protected:
  PipelineTextTest()
      : env_(base::test::TaskEnvironment::TimeSource::kRealTime),
        video_thread_("avbase-txt-S3"),
        audio_thread_("avbase-txt-S4") {}

  void SetUp() override {
    // The renderer requires both runners even for an audio-only file (the
    // video side is simply absent); nothing is posted to S3 here.
    ASSERT_TRUE(video_thread_.Start());
    ASSERT_TRUE(audio_thread_.Start());
  }

  template <typename Pred>
  bool PumpUntil(Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
      PumpRound();
      if (pred()) {
        return true;
      }
    }
    PumpRound();
    return pred();
  }

  void TearDown() override {
    if (pipeline_ && pipeline_->IsRunning()) {
      pipeline_->Stop();
      EXPECT_TRUE(PumpUntil([this] { return !pipeline_->IsRunning(); }));
    }
    env_.RunUntilIdle();
    video_thread_.Stop();
    audio_thread_.Stop();
  }

  void PumpRound() {
    env_.RunUntilIdle();
    if (!pipeline_ || !pipeline_->IsRunning()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    test::FakeAudioSink* audio = audio_sinks_->last_sink();
    if (!audio) {
      return;
    }
    audio->set_render_runner(audio_thread_.task_runner());
    for (int i = 0; i < 3; ++i) {
      audio->PullPeriod(bus_.get());
    }
  }

  void StartPipeline() {
    // Member, not a local: MemoryDataSource references the bytes without
    // copying them.
    media_bytes_ = ReadFileBytes("audio_two_subs.mkv");
    ASSERT_GT(media_bytes_.size(), 10000u);
    auto source = base::MakeRefCounted<MemoryDataSource>(media_bytes_.data(),
                                                         media_bytes_.size());
    av_sync_ = std::make_shared<AvSyncController>(
        AvSyncController::MasterType::kAudio, &tick_clock_,
        AvSyncController::Thresholds());
    DefaultRendererFactory::Deps deps;
    deps.video_task_runner = video_thread_.task_runner();
    deps.audio_task_runner = audio_thread_.task_runner();
    deps.tick_clock = &tick_clock_;
    deps.audio_decoder_factories.push_back(
        base::MakeRefCounted<FFmpegAudioDecoderFactory>(
            audio_thread_.task_runner()));
    deps.text_decoder_factory =
        base::MakeRefCounted<FFmpegTextDecoderFactory>();
    deps.audio_sink_factory = audio_sinks_;
    deps.audio_frames_per_buffer = kFramesPerBuffer;
    deps.av_sync = av_sync_;
    renderer_factory_ = std::make_unique<DefaultRendererFactory>(deps);

    pipeline_ = std::make_unique<PipelineImpl>();
    pipeline_->SetTickClock(&tick_clock_);
    pipeline_->SetClock(av_sync_);
    DemuxerOptions options;
    pipeline_->Start(std::make_unique<FFmpegDemuxer>(media_log_),
                     renderer_factory_.get(), RendererType::kRendererImpl,
                     env_.GetMainThreadTaskRunnerRef(), &client_);
    source_ = std::move(source);
    pipeline_->SetSource(DataSourceDescriptor::FromSource(source_), options);
  }

  void SelectTextTrack(int index, PipelineStatus* out_status,
                       std::atomic<bool>* done) {
    pipeline_->SelectTextTrack(
        index,
        base::BindOnce(
            [](std::atomic<bool>* flag, PipelineStatus* out,
               PipelineStatus s) {
              *out = s;
              flag->store(true);
            },
            done, out_status));
  }

  static constexpr int kTrackEng = 1;
  static constexpr int kTrackChi = 2;

  base::test::TaskEnvironment env_;
  base::Thread video_thread_;
  base::Thread audio_thread_;
  base::DefaultTickClock tick_clock_;
  base::scoped_refptr<MediaLog> media_log_ = base::MakeRefCounted<MediaLog>();
  std::shared_ptr<AvSyncController> av_sync_;
  base::scoped_refptr<DataSource> source_;
  std::vector<uint8_t> media_bytes_;
  std::shared_ptr<test::FakeAudioSinkFactory> audio_sinks_ =
      std::make_shared<test::FakeAudioSinkFactory>();
  std::unique_ptr<DefaultRendererFactory> renderer_factory_;
  test::FakePipelineClient client_;
  std::unique_ptr<PipelineImpl> pipeline_;
  std::unique_ptr<AudioBus> bus_ =
      AudioBus::Create(kAudioChannels, kFramesPerBuffer);
};

TEST_F(PipelineTextTest, NoCuesUntilATrackIsSelected) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }))
      << "never probed the source; events:\n" << client_.EventLog();
  pipeline_->Play();
  // The text leg is off by default: no track selected, no cues, and the
  // pipeline still reaches EOS on the audio track.
  ASSERT_TRUE(PumpUntil([this] { return client_.ended(); }))
      << "audio-only playback did not reach EOS; events:\n"
      << client_.EventLog();
  EXPECT_TRUE(client_.cues().empty());
  EXPECT_FALSE(client_.error());
}

TEST_F(PipelineTextTest, SelectedTrackDeliversItsCues) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));
  pipeline_->Play();

  std::atomic<bool> done{false};
  PipelineStatus status = PipelineStatus::kTrackSwitchError;
  SelectTextTrack(kTrackEng, &status, &done);
  ASSERT_TRUE(PumpUntil([&] { return done.load(); }))
      << "text selection never completed; events:\n" << client_.EventLog();
  EXPECT_EQ(status, PipelineStatus::kOk);

  ASSERT_TRUE(PumpUntil([this] { return client_.cues().size() >= 3; }))
      << "no cues delivered; events:\n" << client_.EventLog();
  // The English track's words, in order. Content equality (not count) is the
  // assertion: it proves both WHICH track was decoded and that the decoder
  // flattened the rects into text.
  ASSERT_EQ(client_.cues().size(), 3u);
  EXPECT_NE(client_.cues()[0].text.find("first track alpha"),
            std::string::npos);
  EXPECT_NE(client_.cues()[1].text.find("first track beta"),
            std::string::npos);
  EXPECT_NE(client_.cues()[2].text.find("first track gamma"),
            std::string::npos);
  // Cue timing came from the container (0.1s, 0.7s, 1.3s starts).
  EXPECT_EQ(client_.cues()[0].pts, base::Milliseconds(100));
  EXPECT_EQ(client_.cues()[0].duration, base::Milliseconds(500));
  EXPECT_FALSE(client_.error());
}

TEST_F(PipelineTextTest, SwitchingToTheSecondTrackDeliversItsCues) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));
  pipeline_->Play();

  std::atomic<bool> done{false};
  PipelineStatus status = PipelineStatus::kTrackSwitchError;
  SelectTextTrack(kTrackEng, &status, &done);
  ASSERT_TRUE(PumpUntil([&] { return done.load(); }));
  ASSERT_TRUE(PumpUntil([this] { return client_.cues().size() >= 3; }));

  done.store(false);
  SelectTextTrack(kTrackChi, &status, &done);
  ASSERT_TRUE(PumpUntil([&] { return done.load(); }))
      << "second selection never completed; events:\n"
      << client_.EventLog();
  EXPECT_EQ(status, PipelineStatus::kOk);
  ASSERT_TRUE(PumpUntil(
      [this] {
        for (const TimedTextCue& cue : client_.cues()) {
          if (cue.text.find("second track alpha") != std::string::npos) {
            return true;
          }
        }
        return false;
      }))
      << "the second track's cues never arrived; events:\n"
      << client_.EventLog();
  EXPECT_FALSE(client_.error());
}

TEST_F(PipelineTextTest, UnknownTextTrackFailsCleanly) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));
  pipeline_->Play();

  std::atomic<bool> done{false};
  PipelineStatus status = PipelineStatus::kOk;
  SelectTextTrack(42, &status, &done);
  ASSERT_TRUE(PumpUntil([&] { return done.load(); }));
  EXPECT_EQ(status, PipelineStatus::kTrackSwitchError);
  EXPECT_TRUE(client_.cues().empty());
  EXPECT_FALSE(client_.error());
}

}  // namespace
}  // namespace avbase::media
