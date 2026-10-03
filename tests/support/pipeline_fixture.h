// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_TESTS_SUPPORT_PIPELINE_FIXTURE_H_
#define AVBASE_TESTS_SUPPORT_PIPELINE_FIXTURE_H_

// The shared pipeline-level test fixture. This replaced four copy-pasted
// fixture classes (pipeline_seek / _throttle / _track / _text unittests)
// whose divergence was cosmetic -- and whose bugs were not: the
// MemoryDataSource dangling-bytes bug and the pull-after-teardown race each
// had to be found and fixed once PER COPY before this existed. Fix the base,
// every pipeline test inherits the fix.
//
// Two modes:
//   * ffmpeg mode (default): FFmpegDemuxer over a DataSource (a testdata
//     file through MemoryDataSource by default) with FFmpeg decoder
//     factories. Requires AVBASE_TESTDATA_DIR.
//   * synthetic mode: SyntheticDemuxer + synthetic decoder factories, for
//     suites that must run without FFmpeg (design goal G2).
//
// Deliberately a plain header-only base: the suites compile it into their
// own binaries, so no new target and no CMake churn.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "avbase/BuildConfig.h"
#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/threading/thread.h"
#include "base/time/default_tick_clock.h"
#include "gtest/gtest.h"
#include "media/base/audio_bus.h"
#include "media/base/data_source.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/pipeline_status.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/pipeline_impl.h"
#include "media/renderers/default_renderer_factory.h"
#if AVBASE_ENABLE_FFMPEG
#include "media/filters/ffmpeg_decoder_factories.h"
#include "media/filters/ffmpeg_demuxer.h"
#include "media/filters/ffmpeg_text_decoder.h"
#endif
#include "tests/support/fake_pipeline_client.h"
#include "tests/support/fake_renderer_sinks.h"
#include "tests/support/fake_sink_factories.h"
#include "tests/support/synthetic_decoders.h"
#include "tests/support/synthetic_demuxer.h"

#ifdef AVBASE_TESTDATA_DIR
#define AVBASE_PIPELINE_FIXTURE_HAS_TESTDATA 1
#else
#define AVBASE_PIPELINE_FIXTURE_HAS_TESTDATA 0
#endif

namespace avbase::media {

class PipelineTestFixture : public ::testing::Test {
 protected:
  static constexpr int kFramesPerBuffer = 256;
  static constexpr int kAudioChannels = 2;
  // Media time one pump round consumes: three audio device periods of
  // kFramesPerBuffer at 48 kHz. 3 * 256 / 48000 = 16 ms, which is also one
  // display interval at 60 Hz -- the two endpoints advance together in a
  // real player, so a realtime-paced round advances both by the same amount.
  static constexpr int kRealtimeRoundMs = 16;
  static constexpr auto kWaitTimeout = std::chrono::seconds(60);

  explicit PipelineTestFixture(bool ffmpeg_mode) : ffmpeg_mode_(ffmpeg_mode) {}

  void SetUp() override {
    runner_ = env_.GetMainThreadTaskRunnerRef();
    ASSERT_TRUE(video_thread_.Start());
    ASSERT_TRUE(audio_thread_.Start());
  }

  // Every wait pumps S1 as it polls: PipelineImpl posts its whole graph
  // build onto the media runner, so a test that only sleeps would wait
  // forever with an empty event log.
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
      EXPECT_TRUE(PumpUntil([this] { return !pipeline_->IsRunning(); }))
          << "pipeline did not stop";
    }
    // Drain whatever teardown posted to S1 before the sub-renderer threads
    // die underneath it.
    env_.RunUntilIdle();
    video_thread_.Stop();
    audio_thread_.Stop();
  }

  // ---- configuration hooks (set before StartPipeline) ----------------------

  // Testdata file name for ffmpeg mode (memory-backed by default).
  std::string media_file_;
  // Enables the FFmpeg text-decoder factory (text-leg suites).
  bool with_text_factory_ = false;

  // ---- shared state ---------------------------------------------------------

  base::test::TaskEnvironment env_{
      base::test::TaskEnvironment::TimeSource::kRealTime};
  base::Thread video_thread_{"avbase-pipe-S3"};
  base::Thread audio_thread_{"avbase-pipe-S4"};
  base::DefaultTickClock tick_clock_;
  base::scoped_refptr<MediaLog> media_log_ = base::MakeRefCounted<MediaLog>();
  std::shared_ptr<AvSyncController> av_sync_;
  base::scoped_refptr<base::SequencedTaskRunner> runner_;
  std::shared_ptr<test::FakeVideoSinkFactory> video_sinks_ =
      std::make_shared<test::FakeVideoSinkFactory>();
  std::shared_ptr<test::FakeAudioSinkFactory> audio_sinks_ =
      std::make_shared<test::FakeAudioSinkFactory>();
  std::unique_ptr<DefaultRendererFactory> renderer_factory_;
  test::FakePipelineClient client_;
  std::unique_ptr<PipelineImpl> pipeline_;
  std::unique_ptr<AudioBus> bus_ =
      AudioBus::Create(kAudioChannels, kFramesPerBuffer);
  test::SyntheticSpec spec_;
  // Member, not a local: MemoryDataSource references the bytes without
  // copying them, so a local vector would dangle the moment the builder
  // returned. The same hazard applies to REASSIGNING it, which is why
  // StartPipeline() below loads only when it is still empty.
  std::vector<uint8_t> media_bytes_;

  // Loads |media_file_| into |media_bytes_| for tests that wrap the source
  // (throttled sources etc.) before calling StartPipeline(custom).
  //
  // Idempotent on purpose. A test that wraps the bytes builds its
  // MemoryDataSource over media_bytes_.data() and then calls
  // StartPipeline(custom_source); if the fixture reloaded the file at that
  // point, the vector would reallocate and every pointer the caller already
  // held would dangle. The throttle suite hit exactly that -- reading freed
  // memory, which FFmpeg duly reported as 46 rounds of "Invalid data found
  // when processing input" and the suite misread as a pacing bug.
  void LoadMediaBytes() {
#if AVBASE_PIPELINE_FIXTURE_HAS_TESTDATA
    if (media_bytes_.empty()) {
      media_bytes_ = ReadMediaFile(media_file_.c_str());
    }
#endif
  }

  // Builds the graph and starts it. |custom_source| overrides the default
  // testdata-file source (throttled sources etc.); synthetic mode ignores it.
  void StartPipeline(base::scoped_refptr<DataSource> custom_source = nullptr) {
    av_sync_ = std::make_shared<AvSyncController>(
        AvSyncController::MasterType::kAudio, &tick_clock_,
        AvSyncController::Thresholds());
    DefaultRendererFactory::Deps deps;
    deps.video_task_runner = video_thread_.task_runner();
    deps.audio_task_runner = audio_thread_.task_runner();
    deps.tick_clock = &tick_clock_;
    if (ffmpeg_mode_) {
#if AVBASE_ENABLE_FFMPEG && AVBASE_PIPELINE_FIXTURE_HAS_TESTDATA
      deps.video_decoder_factories.push_back(
          base::MakeRefCounted<FFmpegVideoDecoderFactory>(
              video_thread_.task_runner()));
      deps.audio_decoder_factories.push_back(
          base::MakeRefCounted<FFmpegAudioDecoderFactory>(
              audio_thread_.task_runner()));
      if (with_text_factory_) {
        deps.text_decoder_factory =
            base::MakeRefCounted<FFmpegTextDecoderFactory>();
      }
#endif
    } else {
      deps.video_decoder_factories.push_back(
          base::MakeRefCounted<test::SyntheticVideoDecoderFactory>(spec_));
      deps.audio_decoder_factories.push_back(
          base::MakeRefCounted<test::SyntheticAudioDecoderFactory>(spec_));
    }
    deps.video_sink_factory = video_sinks_;
    deps.audio_sink_factory = audio_sinks_;
    deps.audio_frames_per_buffer = kFramesPerBuffer;
    deps.av_sync = av_sync_;
    renderer_factory_ = std::make_unique<DefaultRendererFactory>(deps);

    pipeline_ = std::make_unique<PipelineImpl>();
    pipeline_->SetTickClock(&tick_clock_);
    pipeline_->SetClock(av_sync_);
    if (ffmpeg_mode_) {
#if AVBASE_ENABLE_FFMPEG && AVBASE_PIPELINE_FIXTURE_HAS_TESTDATA
      // Load only when nobody has yet: a caller that wrapped the bytes (see
      // LoadMediaBytes) is holding pointers INTO media_bytes_, and
      // reassigning the vector here would reallocate it out from under them.
      if (media_bytes_.empty()) {
        media_bytes_ = ReadMediaFile(media_file_.c_str());
      }
      ASSERT_GT(media_bytes_.size(), 10000u)
          << "testdata file too small: " << media_file_;
      auto source = base::MakeRefCounted<MemoryDataSource>(media_bytes_.data(),
                                                           media_bytes_.size());
      source_ = custom_source ? custom_source : source;
#endif
    } else {
      source_.reset();  // SyntheticDemuxer needs no DataSource.
    }
    DemuxerOptions options;
    if (ffmpeg_mode_) {
#if AVBASE_ENABLE_FFMPEG && AVBASE_PIPELINE_FIXTURE_HAS_TESTDATA
      pipeline_->Start(std::make_unique<FFmpegDemuxer>(media_log_),
                       renderer_factory_.get(), RendererType::kRendererImpl,
                       runner_, &client_);
      pipeline_->SetSource(DataSourceDescriptor::FromSource(source_), options);
#endif
    } else {
      pipeline_->Start(std::make_unique<test::SyntheticDemuxer>(spec_),
                       renderer_factory_.get(), RendererType::kRendererImpl,
                       runner_, &client_);
    }
  }

  void Play() {
    // kReady is signalled by kHaveMetadata (pipeline_impl.cc sets the state
    // right before emitting it). Play() before that is a documented no-op.
    ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }))
        << "never reached kHaveMetadata; events:\n"
        << client_.EventLog();
    pipeline_->Play();
  }

  // Advances playback the way a device would: three audio periods (one
  // display interval at 48 kHz) and one display frame per round. Safe before
  // the sinks exist and after stop (it only drains S1). The 4 ms pause is
  // deliberately SHORTER than a display interval: the audio clock
  // extrapolates between consumption reports, so wall time running AHEAD of
  // consumed media time makes the compositor mis-judge frames as late
  // (measured in the seek suite); lagging is the safe direction.
  int PumpRound() {
    // Always drain S1 first: teardown waits pump through here too, and the
    // stop completion only lands when S1 runs.
    env_.RunUntilIdle();
    if (!pipeline_ || !pipeline_->IsRunning()) {
      // The stop path tears down the renderer together with the fake sinks;
      // pulling past this point would race freed objects.
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    return PullOneInterval();
  }

  // The same pull, but paced so that WALL time tracks MEDIA time -- a device
  // and a display consume at the rate the content runs, and the shared
  // PumpRound deliberately does not (see its 4 ms note above).
  //
  // Why the throttle suite needs it: a throttled DataSource refills at its
  // configured byte rate, so a pump that drains three audio periods per 4 ms
  // (~4x realtime) empties the queues far faster than the source can fill
  // them. Playback then ends in permanent starvation and kHaveEnough is
  // unreachable -- the recovery edge the test is asserting cannot occur at any
  // speed the fixture can assert deterministically. Pacing the pull to the
  // media clock makes consumption and refill comparable, which is the regime
  // a stall-recovery cycle actually lives in.
  //
  // |media_per_round| is how much media time one round consumes (three audio
  // periods of kFramesPerBuffer at 48 kHz = 16 ms by default). The sleep is
  // computed from the round's own media cost, so the caller does not have to
  // restate the audio parameters.
  int PumpRoundRealtime(int media_ms_per_round = kRealtimeRoundMs) {
    env_.RunUntilIdle();
    if (!pipeline_ || !pipeline_->IsRunning()) {
      return 0;
    }
    const auto start = std::chrono::steady_clock::now();
    const int frames = PullOneInterval();
    // Pace to the media cost, minus what the pull itself already took, so a
    // slow round does not push the schedule permanently behind. Never
    // negative: a round that overran sleeps not at all.
    const auto spent = std::chrono::steady_clock::now() - start;
    const auto target = std::chrono::milliseconds(media_ms_per_round) - spent;
    if (target > std::chrono::milliseconds(0)) {
      std::this_thread::sleep_for(target);
    }
    return frames;
  }

  // One display interval's worth of video plus three audio device periods,
  // marshalled onto the render threads. Factored out of PumpRound so the
  // realtime-paced variant pulls exactly the same work.
  int PullOneInterval() {
    test::FakeAudioSink* audio = audio_sinks_->last_sink();
    test::FakeVideoSink* video = video_sinks_->last_sink();
    if (!audio) {
      return 0;
    }
    // The audio renderer's pump lives on S4; a render callback running
    // inline on this thread would touch decoder state from two sequences at
    // once (TSan caught it). Marshal pulls onto S4, like a real device
    // thread. Idempotent: setting it every round is free and closes the
    // window between sink creation and the first explicit arm.
    audio->set_render_runner(audio_thread_.task_runner());
    for (int i = 0; i < 3; ++i) {
      audio->PullPeriod(bus_.get());
    }
    if (video) {
      const size_t before = video->frames().size();
      video->PullFrames(1);
      return static_cast<int>(video->frames().size() - before);
    }
    return 0;
  }

  base::scoped_refptr<DataSource> source_;

 private:
#if AVBASE_PIPELINE_FIXTURE_HAS_TESTDATA
  static std::vector<uint8_t> ReadMediaFile(const char* name) {
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
#endif

  bool ffmpeg_mode_ = false;
};

}  // namespace avbase::media

#endif  // AVBASE_TESTS_SUPPORT_PIPELINE_FIXTURE_H_
