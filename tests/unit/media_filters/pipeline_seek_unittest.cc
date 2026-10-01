// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The pipeline-level seek contract on the synthetic source (docs/07 §5),
// which no component-level suite can see: a seek issued through Pipeline
// must resynchronize decode, the renderer generation and both sinks, and
// the next frame on the display must be the frame the seek asked for.
//
// This is a rebuild of the harness that reproduced #50 ("after a seek,
// video never resumed"). The first attempt was withdrawn for flakiness
// (docs/PROGRESS.md, tenth round §5.1); this version differs where it
// counted:
//   * every wait is bounded AND one-sided -- the test never sleeps a fixed
//     delay, it polls for a state the pipeline must reach and fails with
//     the event log if the deadline passes;
//   * rendering is advanced only by pumping the two fake sinks, so there
//     is no free-running clock to race against;
//   * the landing assertion accepts frames 150..152: a frame dropped as
//     late is the compositor's legitimate decision, not a defect;
//   * the "no frame from before the seek after the landing frame"
//     assertion uses the demuxer's serial bump, not frame timestamps.
//
// WHY REAL THREADS for S3/S4: production shape, and ~RendererImpl posts
// sub-renderer destruction to those sequences and waits for it -- a
// single-threaded runner deadlocks there (see renderer_impl_unittest.cc).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/task_environment.h"
#include "base/threading/thread.h"
#include "base/time/default_tick_clock.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/audio_bus.h"
#include "media/base/pipeline_status.h"
#include "media/base/video_frame.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/pipeline_impl.h"
#include "media/renderers/default_renderer_factory.h"
#include "tests/support/fake_pipeline_client.h"
#include "tests/support/fake_renderer_sinks.h"
#include "tests/support/fake_sink_factories.h"
#include "tests/support/synthetic_decoders.h"
#include "tests/support/synthetic_demuxer.h"

namespace avbase::media {
namespace {

// Ceiling for every wait. Generous, because it is a ceiling and not a delay:
// the test passes the moment the state is reached.
constexpr auto kWaitTimeout = std::chrono::seconds(10);
// The seek target: 5 s of a 30 fps source is frame 150.
constexpr uint32_t kSeekFrame = 150;
constexpr uint32_t kLandingSlack = 2;
// Audio ring chunk, kept small so a pump round moves a noticeable slice.
constexpr int kFramesPerBuffer = 256;
constexpr int kAudioChannels = 2;

template <typename Pred>
bool WaitFor(Pred pred) {
  const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

class PipelineSeekTest : public ::testing::Test {
 protected:
  PipelineSeekTest()
      : env_(base::test::TaskEnvironment::TimeSource::kRealTime),
        video_thread_("avbase-test-S3"),
        audio_thread_("avbase-test-S4") {}

  void SetUp() override {
    runner_ = env_.GetMainThreadTaskRunnerRef();
    ASSERT_TRUE(video_thread_.Start());
    ASSERT_TRUE(audio_thread_.Start());
  }

  // Every wait pumps S1 as it polls: PipelineImpl posts its whole graph
  // build onto the media runner (Start is "posted, not run inline"), so a
  // test that only sleeps would wait forever with an empty event log.
  template <typename Pred>
  bool PumpUntil(Pred pred) {
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
      env_.RunUntilIdle();
      if (pred()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    env_.RunUntilIdle();
    return pred();
  }

 public:
  // Bound as the seek callback, hence public.
  void OnSeeked() { seeked_.store(true); }

 protected:
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

  // Builds the graph over the synthetic source and starts it.
  void StartPipeline() {
    av_sync_ = std::make_shared<AvSyncController>(
        AvSyncController::MasterType::kAudio, &tick_clock_,
        AvSyncController::Thresholds());

    DefaultRendererFactory::Deps deps;
    deps.video_task_runner = video_thread_.task_runner();
    deps.audio_task_runner = audio_thread_.task_runner();
    deps.tick_clock = &tick_clock_;
    deps.video_decoder_factories.push_back(video_decoders_);
    deps.audio_decoder_factories.push_back(audio_decoders_);
    deps.video_sink_factory = video_sinks_;
    deps.audio_sink_factory = audio_sinks_;
    deps.audio_frames_per_buffer = kFramesPerBuffer;
    deps.av_sync = av_sync_;
    renderer_factory_ = std::make_unique<DefaultRendererFactory>(deps);

    pipeline_ = std::make_unique<PipelineImpl>();
    pipeline_->SetTickClock(&tick_clock_);
    pipeline_->SetClock(av_sync_);
    pipeline_->Start(std::make_unique<test::SyntheticDemuxer>(spec_),
                     renderer_factory_.get(), RendererType::kRendererImpl,
                     runner_, &client_);
  }

  void PlayAndWaitForSinks(AudioBus* bus) {
    // kReady is signalled by kHaveMetadata (pipeline_impl.cc sets the state
    // right before emitting it). Play() before that is a documented no-op --
    // and OnDurationChange, which the Start() contract also names, arrives
    // *before* kReady, so it must not be waited on here.
    ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }))
        << "never reached kHaveMetadata; events:\n" << client_.EventLog();
    pipeline_->Play();
    ASSERT_TRUE(PumpUntil([this, bus] {
      PumpRound(bus);
      return video_sinks_->last_sink() && audio_sinks_->last_sink() &&
             video_sinks_->last_sink()->start_count() > 0 &&
             audio_sinks_->last_sink()->start_count() > 0;
    })) << "sinks never started; events:\n" << client_.EventLog();
    // The audio renderer's pump lives on S4; a render callback running inline
    // on this thread would touch decoder state from two sequences at once
    // (TSan caught it). Marshal pulls onto S4, like a real device thread.
    audio_sinks_->last_sink()->set_render_runner(audio_thread_.task_runner());
  }

  // Advances playback the way a device would: one audio period and one
  // display interval per round. Safe before the sinks exist (it only drains
  // S1); returns the number of frames stored this round.
  int PumpRound(AudioBus* bus) {
    // Play()/seek callbacks/state transitions all move through S1, so a
    // pump round is also an S1 drain.
    env_.RunUntilIdle();
    // Breathing room for the real S3/S4 threads, but deliberately SHORTER
    // than one display interval: the audio clock extrapolates media time by
    // the wall clock between consumption updates, so wall time running AHEAD
    // of consumed media time makes the compositor drop early post-seek frames
    // as late (measured: landing frame drifted to 155..158 at a 16 ms sleep).
    // Lagging is the safe direction -- a held frame is presented when due.
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    test::FakeAudioSink* audio = audio_sinks_->last_sink();
    test::FakeVideoSink* video = video_sinks_->last_sink();
    if (!audio || !video) {
      return 0;
    }
    // Three periods of |kFramesPerBuffer| at 48 kHz are ~16 ms of media time
    // -- one display interval. If the audio clock advances slower than the
    // display cadence, the compositor sees every frame as "not yet due" and
    // the landing frame never presents (that was 18% of the first stress
    // run).
    for (int i = 0; i < 3; ++i) {
      audio->PullPeriod(bus);
    }
    const size_t before = video->frames().size();
    video->PullFrames(1);
    return static_cast<int>(video->frames().size() - before);
  }

  // First stored frame whose index is >= |floor|, or nullptr.
  static const base::scoped_refptr<VideoFrame>* LandingFrame(
      const test::FakeVideoSink& video, uint32_t floor, size_t from) {
    const std::vector<base::scoped_refptr<VideoFrame>>& frames =
        video.frames();
    for (size_t i = from; i < frames.size(); ++i) {
      uint32_t index = 0;
      if (test::ReadFrameIndex(*frames[i], &index) && index >= floor) {
        return &frames[i];
      }
    }
    return nullptr;
  }

  base::test::TaskEnvironment env_;
  base::Thread video_thread_;
  base::Thread audio_thread_;
  base::DefaultTickClock tick_clock_;
  test::SyntheticSpec spec_;
  std::shared_ptr<AvSyncController> av_sync_;
  base::scoped_refptr<base::SequencedTaskRunner> runner_;
  base::scoped_refptr<test::SyntheticVideoDecoderFactory> video_decoders_ =
      base::MakeRefCounted<test::SyntheticVideoDecoderFactory>(spec_);
  base::scoped_refptr<test::SyntheticAudioDecoderFactory> audio_decoders_ =
      base::MakeRefCounted<test::SyntheticAudioDecoderFactory>(spec_);
  std::shared_ptr<test::FakeVideoSinkFactory> video_sinks_ =
      std::make_shared<test::FakeVideoSinkFactory>();
  std::shared_ptr<test::FakeAudioSinkFactory> audio_sinks_ =
      std::make_shared<test::FakeAudioSinkFactory>();
  std::unique_ptr<DefaultRendererFactory> renderer_factory_;
  test::FakePipelineClient client_;
  std::unique_ptr<PipelineImpl> pipeline_;
  std::atomic<bool> seeked_{false};
};

TEST_F(PipelineSeekTest, SeekToFiveSecondsLandsOnFrame150) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.Started(); }))
      << "pipeline never started; events:\n" << client_.EventLog();
  ASSERT_FALSE(client_.HasError())
      << client_.error().ToString() << "\n" << client_.EventLog();
  auto bus = AudioBus::Create(kAudioChannels, kFramesPerBuffer);
  PlayAndWaitForSinks(bus.get());

  // Get playback under way: pump roughly a second, and require that real
  // frames came out -- a renderer that never presented would make the seek
  // assertions below vacuous.
  int presented_before = 0;
  for (int round = 0; round < 60; ++round) {
    presented_before += PumpRound(bus.get());
  }
  ASSERT_GT(presented_before, 0) << client_.EventLog();

  const size_t first_batch = video_sinks_->last_sink()->frames().size();
  seeked_.store(false);
  pipeline_->Seek(base::Seconds(5), base::BindOnce(&PipelineSeekTest::OnSeeked,
                                                   base::Unretained(this)));
  ASSERT_TRUE(PumpUntil([this, &bus] {
    PumpRound(bus.get());
    return seeked_.load();
  })) << "seek callback never ran; events:\n" << client_.EventLog();
  EXPECT_GE(video_sinks_->last_sink()->flush_count(), 1);

  // The landing frame: the first presented frame at or after frame 150.
  const test::FakeVideoSink* video = video_sinks_->last_sink();
  ASSERT_TRUE(PumpUntil([&] {
    PumpRound(bus.get());
    return LandingFrame(*video, kSeekFrame, first_batch) != nullptr;
  })) << "no frame >= " << kSeekFrame
      << " presented after the seek; events:\n" << client_.EventLog()
      << "sink stats: presented="
      << video->GetStats().frames_presented
      << " dropped=" << video->GetStats().frames_dropped
      << " stored=" << video->frames().size();

  const base::scoped_refptr<VideoFrame>* landed =
      LandingFrame(*video, kSeekFrame, first_batch);
  uint32_t index = 0;
  ASSERT_TRUE(test::ReadFrameIndex(**landed, &index));
  EXPECT_LE(index, kSeekFrame + kLandingSlack)
      << "landed on frame " << index << ", too far past the target";

  // No frame from before the seek may follow the landing frame. The
  // compositor legitimately drops late frames, so the boundary is "once the
  // new generation is on the display, the old one is gone for good".
  const size_t landed_at =
      static_cast<size_t>(landed - video->frames().data());
  for (int round = 0; round < 120; ++round) {
    PumpRound(bus.get());
  }
  const std::vector<base::scoped_refptr<VideoFrame>>& frames =
      video->frames();
  for (size_t i = landed_at; i < frames.size(); ++i) {
    uint32_t after = 0;
    if (test::ReadFrameIndex(*frames[i], &after)) {
      EXPECT_GE(after, kSeekFrame)
          << "pre-seek frame " << after << " presented at slot " << i
          << ", after the landing frame";
    }
  }
  EXPECT_FALSE(client_.HasError())
      << client_.error().ToString() << "\n" << client_.EventLog();
}

// The M9 accurate-seek contract at the pipeline level: with the drop window
// open, NOTHING before the target frame may reach the display -- not the
// pre-seek generation, not the post-seek frames before the target. That is
// strictly stronger than the keyframe seek above, whose landing assertion
// had to tolerate frames beyond the target but could not bound the floor.
// The 1-in-6 recovery miss was the rendering_ flag never being set (a lost
// patch hunk, see PROGRESS round 13) -- with it fixed and the per-present
// check in place, 10 stress runs are clean. The throttle test's "EOS before
// starvation" was the same bug.
TEST_F(PipelineSeekTest, AccurateSeekPresentsNothingBeforeTheTarget) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.Started(); }))
      << "pipeline never started; events:\n" << client_.EventLog();
  auto bus = AudioBus::Create(kAudioChannels, kFramesPerBuffer);
  PlayAndWaitForSinks(bus.get());
  int presented_before = 0;
  for (int round = 0; round < 60; ++round) {
    presented_before += PumpRound(bus.get());
  }
  ASSERT_GT(presented_before, 0) << client_.EventLog();

  std::atomic<bool> reached{false};
  // Open the window BEFORE the keyframe seek: it survives Flush() by design,
  // which is what frames the new generation. On the synthetic 30 fps source,
  // 5 s is frame 150, and the pre-seek position (~1 s) is frame ~30 -- the
  // window must drop those too.
  pipeline_->BeginAccurateSeek(
      base::Seconds(5),
      base::BindOnce([](std::atomic<bool>* r) { r->store(true); }, &reached));
  const size_t window_batch = video_sinks_->last_sink()
                                  ? video_sinks_->last_sink()->frames().size()
                                  : 0;
  seeked_.store(false);
  pipeline_->Seek(base::Seconds(5), base::BindOnce(&PipelineSeekTest::OnSeeked,
                                                   base::Unretained(this)));
  ASSERT_TRUE(PumpUntil([this, &bus] {
    PumpRound(bus.get());
    return seeked_.load();
  })) << "seek callback never ran; events:\n" << client_.EventLog();

  const test::FakeVideoSink* video = video_sinks_->last_sink();
  ASSERT_TRUE(PumpUntil([&] {
    PumpRound(bus.get());
    return LandingFrame(*video, kSeekFrame, window_batch) != nullptr;
  })) << "no frame >= " << kSeekFrame
      << " presented after the accurate seek; events:\n"
      << client_.EventLog();

  // THE contract: from the moment the window opened, no frame below the
  // target was presented. (A frame dropped as late can land past the target,
  // so the upper bound keeps the keyframe test's slack; the floor is exact.)
  const std::vector<base::scoped_refptr<VideoFrame>>& frames =
      video->frames();
  for (size_t i = window_batch; i < frames.size(); ++i) {
    uint32_t index = 0;
    if (test::ReadFrameIndex(*frames[i], &index)) {
      EXPECT_GE(index, kSeekFrame)
          << "frame " << index << " presented under an open accurate-seek "
          << "window targeting frame " << kSeekFrame;
    }
  }
  uint32_t landed_index = 0;
  ASSERT_TRUE(test::ReadFrameIndex(
      **LandingFrame(*video, kSeekFrame, window_batch), &landed_index));
  EXPECT_LE(landed_index, kSeekFrame + kLandingSlack);

  // The window's reached callback must have fired -- the facade's
  // SeekController completes the user's seek on it, so a silent window would
  // hang every accurate seek.
  ASSERT_TRUE(PumpUntil([&] {
    PumpRound(bus.get());
    return reached.load();
  })) << "accurate-seek window never reported reaching the target";

  // The seek's flush drains both queues; the starvation signal (M9) must
  // have reported the dry edge and the recovery to the client, in that
  // order of causality. Without the renderer's CheckBufferingTransitions,
  // have_nothing stays false forever and the facade's HWM never advances.
  EXPECT_TRUE(client_.have_nothing())
      << "no kHaveNothing edge after the seek; events:\n"
      << client_.EventLog();
  EXPECT_TRUE(client_.have_enough())
      << "no kHaveEnough edge after the seek; events:\n"
      << client_.EventLog();
  EXPECT_FALSE(client_.HasError())
      << client_.error().ToString() << "\n" << client_.EventLog();
}

}  // namespace
}  // namespace avbase::media
