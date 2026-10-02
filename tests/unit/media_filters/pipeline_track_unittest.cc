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
#include "media/filters/ffmpeg_decoder_factories.h"
#include "media/filters/ffmpeg_demuxer.h"
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

// Phase 4: runtime audio-track switching. The fixture mirrors
// pipeline_throttle_unittest.cc; the media is a 3 s file with one video and
// TWO audio tracks (tests/testdata/two_audio_tracks.m4a), so a switch has a
// real alternate to hand over to and playback can continue to EOS afterwards.
class PipelineTrackTest : public ::testing::Test {
 protected:
  PipelineTrackTest()
      : env_(base::test::TaskEnvironment::TimeSource::kRealTime),
        video_thread_("avbase-trk-S3"),
        audio_thread_("avbase-trk-S4") {}

  void SetUp() override {
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
      // The stop path tears down the renderer together with the fake sinks;
      // pulling past this point would race freed objects.
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    test::FakeAudioSink* audio = audio_sinks_->last_sink();
    test::FakeVideoSink* video = video_sinks_->last_sink();
    if (!audio || !video) {
      return;
    }
    // Idempotent, from the first pull: the direct-call path in the sink races
    // the renderer teardown, so every pull must go through the renderer's own
    // sequence (the same reason the seek/throttle fixtures arm this once).
    audio->set_render_runner(audio_thread_.task_runner());
    for (int i = 0; i < 3; ++i) {
      audio->PullPeriod(bus_.get());
    }
    video->PullFrames(1);
  }

  void StartPipeline() {
    // Member, not a local: MemoryDataSource references the bytes without
    // copying them, so a local vector would dangle the moment this function
    // returned. (The DISABLED throttle fixture carries the same latent bug.)
    media_bytes_ = ReadFileBytes("two_audio_tracks.m4a");
    ASSERT_GT(media_bytes_.size(), 50000u);
    auto source = base::MakeRefCounted<MemoryDataSource>(
        media_bytes_.data(), media_bytes_.size());
    av_sync_ = std::make_shared<AvSyncController>(
        AvSyncController::MasterType::kAudio, &tick_clock_,
        AvSyncController::Thresholds());
    DefaultRendererFactory::Deps deps;
    deps.video_task_runner = video_thread_.task_runner();
    deps.audio_task_runner = audio_thread_.task_runner();
    deps.tick_clock = &tick_clock_;
    deps.video_decoder_factories.push_back(
        base::MakeRefCounted<FFmpegVideoDecoderFactory>(
            video_thread_.task_runner()));
    deps.audio_decoder_factories.push_back(
        base::MakeRefCounted<FFmpegAudioDecoderFactory>(
            audio_thread_.task_runner()));
    deps.video_sink_factory = video_sinks_;
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

  // The container stream indices of the two audio tracks in the test media.
  static constexpr int kTrackA = 0;
  static constexpr int kTrackB = 1;

  base::test::TaskEnvironment env_;
  base::Thread video_thread_;
  base::Thread audio_thread_;
  base::DefaultTickClock tick_clock_;
  base::scoped_refptr<MediaLog> media_log_ = base::MakeRefCounted<MediaLog>();
  std::shared_ptr<AvSyncController> av_sync_;
  base::scoped_refptr<DataSource> source_;
  std::vector<uint8_t> media_bytes_;
  std::shared_ptr<test::FakeVideoSinkFactory> video_sinks_ =
      std::make_shared<test::FakeVideoSinkFactory>();
  std::shared_ptr<test::FakeAudioSinkFactory> audio_sinks_ =
      std::make_shared<test::FakeAudioSinkFactory>();
  std::unique_ptr<DefaultRendererFactory> renderer_factory_;
  test::FakePipelineClient client_;
  std::unique_ptr<PipelineImpl> pipeline_;
  std::unique_ptr<AudioBus> bus_ =
      AudioBus::Create(kAudioChannels, kFramesPerBuffer);
};

TEST_F(PipelineTrackTest, DemuxerEnumeratesBothAudioTracks) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }))
      << "never probed the source; events:\n" << client_.EventLog();
  // Serialize test-thread pulls onto the renderer's own sequence; without
  // this the direct PullPeriod path races the renderer teardown (the same
  // arm-the-sink step the seek and throttle fixtures do).
  if (audio_sinks_->last_sink()) {
    audio_sinks_->last_sink()->set_render_runner(audio_thread_.task_runner());
  }

  // Player::SelectTrack validates the requested index against
  // MediaInfo::streams, so both tracks must be visible there with their
  // container indices. (The demuxer-side GetStreams() override is exercised
  // by the switch tests below: without it, SelectAudioTrack finds no
  // alternate and fails.)
  const MediaInfo info = pipeline_->media_info();
  const std::vector<const StreamInfo*> audio =
      info.StreamsOfKind(StreamKind::kAudio);
  ASSERT_EQ(audio.size(), 2u);
  EXPECT_EQ(audio[0]->index, kTrackA);
  EXPECT_EQ(audio[1]->index, kTrackB);
}

// Regression anchor for the pre-switch state: an audio-only container with
// two tracks must play to EOS with no track switch at all (the demuxer now
// routes only the active stream; a wedged demux loop shows up here first).
TEST_F(PipelineTrackTest, PlaysAudioOnlyTwoTrackFileToEosWithoutSwitch) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));
  if (audio_sinks_->last_sink()) {
    audio_sinks_->last_sink()->set_render_runner(audio_thread_.task_runner());
  }
  pipeline_->Play();
  ASSERT_TRUE(PumpUntil([this] { return client_.ended(); }))
      << "no-switch playback did not reach EOS; events:\n"
      << client_.EventLog();
}

TEST_F(PipelineTrackTest, SwitchAudioTrackMidPlayAndReachEos) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }))
      << "never probed the source; events:\n" << client_.EventLog();
  // Serialize test-thread pulls onto the renderer's own sequence; without
  // this the direct PullPeriod path races the renderer teardown (the same
  // arm-the-sink step the seek and throttle fixtures do).
  if (audio_sinks_->last_sink()) {
    audio_sinks_->last_sink()->set_render_runner(audio_thread_.task_runner());
  }
  pipeline_->Play();
  // No HaveEnough gate here: the buffering verdict only fires on a
  // DRY->RECOVER edge, and a fixture that pumps faster than realtime never
  // goes dry. Playing is proven by reaching EOS below.

  std::atomic<bool> switched{false};
  PipelineStatus switch_status = PipelineStatus::kTrackSwitchError;
  pipeline_->SelectAudioTrack(
      kTrackB,
      base::BindOnce(
          [](std::atomic<bool>* flag, PipelineStatus* out, PipelineStatus s) {
            *out = s;
            flag->store(true);
          },
          &switched, &switch_status));
  ASSERT_TRUE(PumpUntil([&] { return switched.load(); }))
      << "track switch never completed; events:\n" << client_.EventLog();
  EXPECT_EQ(switch_status, PipelineStatus::kOk);
  EXPECT_FALSE(client_.error()) << client_.error().ToString();

  // Playback continues on the new track and reaches the end of the 3 s file.
  ASSERT_TRUE(PumpUntil([this] { return client_.ended(); }))
      << "playback did not reach EOS after the switch; events:\n"
      << client_.EventLog();
}

TEST_F(PipelineTrackTest, SwitchingBackToTheFirstTrackAlsoWorks) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));
  if (audio_sinks_->last_sink()) {
    audio_sinks_->last_sink()->set_render_runner(audio_thread_.task_runner());
  }
  pipeline_->Play();

  for (const int target : {kTrackB, kTrackA, kTrackB}) {
    std::atomic<bool> switched{false};
    PipelineStatus status = PipelineStatus::kTrackSwitchError;
    pipeline_->SelectAudioTrack(
        target,
        base::BindOnce(
            [](std::atomic<bool>* flag, PipelineStatus* out, PipelineStatus s) {
              *out = s;
              flag->store(true);
            },
            &switched, &status));
    ASSERT_TRUE(PumpUntil([&] { return switched.load(); }))
        << "switch to " << target << " never completed";
    EXPECT_EQ(status, PipelineStatus::kOk);
    EXPECT_FALSE(client_.error());
  }
}

TEST_F(PipelineTrackTest, UnknownStreamIndexFailsWithoutErrorState) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));

  std::atomic<bool> done{false};
  PipelineStatus status = PipelineStatus::kOk;
  pipeline_->SelectAudioTrack(
      99,
      base::BindOnce(
          [](std::atomic<bool>* flag, PipelineStatus* out, PipelineStatus s) {
            *out = s;
            flag->store(true);
          },
          &done, &status));
  ASSERT_TRUE(PumpUntil([&] { return done.load(); }));
  EXPECT_EQ(status, PipelineStatus::kTrackSwitchError);
  // A rejected switch is not a playback failure: the pipeline stays healthy.
  EXPECT_FALSE(client_.error());
}

}  // namespace
}  // namespace avbase::media
