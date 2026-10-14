// Copyright 2026 The avbase authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Options that travel from RendererImpl::Deps into the sub-renderers, asserted
// at the point where they have to have arrived.
//
// WHY THIS FILE EXISTS. "max-fps" and "framedrop" were listed as migrated in
// docs/05 with a ✅, and neither did anything. The chain looked complete --
// option_registry wrote PlayerConfig, VideoRendererImpl::SetMaxFps/
// SetMaxFrameDrop existed and forwarded to the compositor -- but nothing ever
// CALLED them, so the compositor's max_fps_ sat at its default 0 and the cap
// never applied. The algorithm underneath is the best-tested code in the
// project (20+ cases in video_frame_compositor_unittest.cc), which is exactly
// why the gap survived: every test that ran set in.max_fps directly, and none
// asked whether the option reached it.
//
// So the assertions here observe BOTH ends of the segment that was missing:
//   * the value arrives (max_fps() / max_frame_drop() read back through the
//     renderer), and
//   * arriving changes behaviour (the fps-capped drop counter moves, and the
//     setting survives a video track switch, which throws the renderer away).
// Neither alone is enough: a value that arrives but does nothing is the same
// bug in a different place, and a counter that moves could be moving for some
// other reason -- hence the paired disabled case.
//
// The file also covers "vn"/"an", which DO work and had no test at all. That is
// not a hypothetical: while wiring the cap above, those two assignments were
// deleted by accident and all 528 tests in this configuration stayed green.
// An option that works but is asserted nowhere is one edit away from not
// working, and nothing would say so.
//
// WHAT THIS FILE DOES NOT COVER, which the mutation run made plain. Deleting
// `factory_deps.video_disabled = config_.video.disabled;` from
// PlayerImpl::PrepareAsync leaves every test here green, because they build
// RendererImpl::Deps directly and never go through Player. No test in the tree
// constructs a Player at all -- grep PlayerBuilder across tests/ finds nothing
// -- so the hop from PlayerConfig into Deps (this file's subject, one layer up)
// is asserted nowhere. The vn/an tests above therefore pin the media-layer hop
// only. That is recorded as an open gap rather than papered over: a test that
// looked like it covered the player layer would be worse than the honest
// boundary, for the same reason a migration table may not mark a no-op done.

#include "media/filters/renderer_impl.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_decoder_factory.h"
#include "media/base/decoder_buffer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/pipeline_status.h"
#include "media/base/renderer.h"
#include "media/base/video_decoder_factory.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/video_renderer_impl.h"
#include "tests/support/fake_decoder_factories.h"
#include "tests/support/fake_demuxer_stream.h"
#include "tests/support/fake_renderer_sinks.h"
#include "tests/support/mock_renderer_client.h"

namespace avbase::media {
namespace {

constexpr int kFramesPerBuffer = 256;
constexpr auto kWaitTimeout = std::chrono::seconds(5);

// ijkplayer's own default for max-fps, so this is the value a host gets
// without setting anything -- which is why it has to work by default.
constexpr int kCap = 31;

class MaxFpsOptionTest : public ::testing::Test {
 protected:
  MaxFpsOptionTest()
      : env_(base::test::TaskEnvironment::TimeSource::kRealTime),
        video_thread_("avbase-test-S3"),
        audio_thread_("avbase-test-S4") {}

  void SetUp() override {
    runner_ = env_.GetMainThreadTaskRunnerRef();
    ASSERT_TRUE(video_thread_.Start());
    ASSERT_TRUE(audio_thread_.Start());
  }

  void TearDown() override {
    DestroyRenderer();
    video_thread_.Stop();
    audio_thread_.Stop();
  }

  // Video-only by default: the fps cap is a video policy, and an audio leg
  // would only add a second clock that has to be kept running for the video one
  // to matter. The vn/an tests override the two disabled flags.
  RendererImpl::Deps MakeDeps(int max_fps, int max_frame_drop) {
    return MakeDeps(max_fps, max_frame_drop, /*video_disabled=*/false,
                    /*audio_disabled=*/true);
  }

  RendererImpl::Deps MakeDeps(int max_fps, int max_frame_drop,
                              bool video_disabled, bool audio_disabled) {
    RendererImpl::Deps deps;
    deps.media_task_runner = runner_;
    deps.tick_clock = env_.GetTickClock();
    deps.audio_frames_per_buffer = kFramesPerBuffer;
    deps.video_disabled = video_disabled;
    deps.audio_disabled = audio_disabled;
    deps.video_task_runner = video_thread_.task_runner();
    deps.audio_task_runner = audio_thread_.task_runner();
    deps.video_max_fps = max_fps;
    deps.video_max_frame_drop = max_frame_drop;
    behaviour_.outputs_per_buffer = 2;
    video_factory_ =
        base::MakeRefCounted<test::FakeVideoDecoderFactory>(behaviour_);
    deps.video_factories.emplace_back(video_factory_);
    auto sink = std::make_unique<test::FakeVideoSink>();
    video_sink_ = sink.get();
    deps.video_sink = std::move(sink);
    av_sync_ = std::make_shared<AvSyncController>(
        AvSyncController::MasterType::kAudio, env_.GetTickClock(),
        AvSyncController::Thresholds());
    deps.av_sync = av_sync_;
    return deps;
  }

  void CreateRenderer(int max_fps, int max_frame_drop) {
    renderer_ =
        std::make_unique<RendererImpl>(MakeDeps(max_fps, max_frame_drop));
  }

  // Installs a video stream with scripted buffers and returns the raw pointer
  // the resource now hands out. A second call is how a test gets a DIFFERENT
  // stream, which is what the track-switch path needs -- OnTracksChanged is a
  // no-op for the SAME stream.
  //
  // The buffer count is deliberately generous. A capped source presents at most
  // max_fps frames per second of buffered material, so a short stream can run
  // out of frames before the cap has dropped anything -- which turns "wait for
  // a capped drop" into a race against the end of the data rather than a test
  // of the cap. 400 buffers at 2 outputs each is several seconds of material.
  DemuxerStream* AddVideoStream() {
    auto stream = std::make_unique<test::FakeDemuxerStream>(
        DemuxerStreamType::kVideo, test::MakeValidAudioConfig(),
        test::MakeValidVideoConfig());
    for (int i = 0; i < 400; ++i) {
      stream->AppendBuffer(
          test::MakeDataBuffer(DemuxerStreamType::kVideo, stream->serial()));
    }
    DemuxerStream* raw = stream.get();
    resource_.set_stream(DemuxerStreamType::kVideo, raw);
    video_streams_.push_back(std::move(stream));
    return raw;
  }

  void Initialize() {
    renderer_->Initialize(&resource_, &client_, runner_,
                          base::BindOnce(&MaxFpsOptionTest::OnInitialized,
                                         base::Unretained(this)));
  }

  void OnInitialized(PipelineStatus status) {
    init_done_ = true;
    init_status_ = status;
  }

  // Pumps the media sequence and pulls frames from the sink the way a running
  // display would, until |predicate| holds. Bounded: a stall fails rather than
  // hangs (the discipline decoder_stream_unittest.cc set).
  template <typename Predicate>
  bool WaitFor(Predicate predicate,
               std::chrono::milliseconds timeout = kWaitTimeout) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
      env_.RunUntilIdle();
      if (predicate()) {
        return true;
      }
      if (video_sink_) {
        video_sink_->PullFrames(8);
      }
      if (std::chrono::steady_clock::now() - start > timeout) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void DestroyRenderer() {
    if (!renderer_) {
      return;
    }
    for (int i = 0; i < 100 && env_.HasDelayedTasks(); ++i) {
      env_.RunUntilIdle();
      if (!env_.HasDelayedTasks()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    env_.RunUntilIdle();
    renderer_.reset();
  }

  uint64_t FpsDropped() {
    return renderer_->GetStatistics().video_frames_dropped_fps;
  }

  uint64_t Presented() {
    return renderer_->GetStatistics().video_frames_presented;
  }

  // Runs the renderer until at least one frame has been presented and playback
  // has started, so the compositor has a last-present time to cap against.
  // Returns false if the pipeline never got there.
  bool StartAndPresentOneFrame() {
    if (!WaitFor([this] { return init_done_; })) {
      return false;
    }
    EXPECT_EQ(init_status_, PipelineStatus::kOk);
    renderer_->StartPlayingFrom(base::TimeDelta());
    return WaitFor([this] { return Presented() > 0; });
  }

  base::test::TaskEnvironment env_;
  base::Thread video_thread_;
  base::Thread audio_thread_;
  base::scoped_refptr<base::SequencedTaskRunner> runner_;
  test::FakeDecoderBehaviour behaviour_;
  base::scoped_refptr<test::FakeVideoDecoderFactory> video_factory_;
  std::shared_ptr<AvSyncController> av_sync_;
  test::FakeVideoSink* video_sink_ = nullptr;
  test::FakeMediaResource resource_;
  std::vector<std::unique_ptr<test::FakeDemuxerStream>> video_streams_;
  std::unique_ptr<RendererImpl> renderer_;
  test::FakeRendererClient client_;
  bool init_done_ = false;
  PipelineStatus init_status_ = PipelineStatus::kOk;
};

// The value arrives. Before the wiring this asserted the opposite, because
// nothing ever called SetMaxFps.
TEST_F(MaxFpsOptionTest, TheCapReachesTheCompositor) {
  CreateRenderer(/*max_fps=*/kCap, /*max_frame_drop=*/7);
  AddVideoStream();
  Initialize();
  ASSERT_TRUE(WaitFor([this] { return init_done_; }));
  EXPECT_EQ(init_status_, PipelineStatus::kOk);

  // Read back through the renderer, which reads through the compositor. If the
  // segment from Deps to the algorithm input were cut, both of these are 0.
  VideoRendererImpl* video = renderer_->video_renderer();
  ASSERT_NE(video, nullptr);
  EXPECT_EQ(video->max_fps(), kCap);
  EXPECT_EQ(video->max_frame_drop(), 7);
}

// Arriving changes behaviour: frames arriving faster than the cap are dropped
// rather than displayed, which is what the cap means here (the compositor drops
// instead of stretching the display interval, so the output rate matches the
// cap exactly rather than accumulating A/V drift).
TEST_F(MaxFpsOptionTest, TheCapActuallyCaps) {
  CreateRenderer(/*max_fps=*/kCap, /*max_frame_drop=*/0);
  AddVideoStream();
  Initialize();
  // StartPlayingFrom() and the first presentation both happen in here; the cap
  // cannot engage before the first frame is on screen, so the wait is part of
  // the setup rather than of the assertion.
  ASSERT_TRUE(StartAndPresentOneFrame());

  // Straight "some frame was dropped by the cap", not "more than the count
  // sampled at an arbitrary instant": the sample made this a race against the
  // end of the buffered data, and it failed once in three runs before it was
  // removed. The compositor only ever increments this counter from the cap
  // path, so a non-zero value is already the whole claim.
  ASSERT_TRUE(WaitFor([this] { return FpsDropped() > 0; }))
      << "the cap was set but no frame was ever dropped for it";
}

// The control for the test above: same source, same pump, no cap. If the cap
// were somehow applied by default, this is where it would show up -- and it
// must not, because 0 is what a host that builds a renderer without a Player
// gets.
TEST_F(MaxFpsOptionTest, DisabledCapDropsNothing) {
  CreateRenderer(/*max_fps=*/0, /*max_frame_drop=*/0);
  AddVideoStream();
  Initialize();
  ASSERT_TRUE(StartAndPresentOneFrame());

  const uint64_t baseline = FpsDropped();
  EXPECT_EQ(baseline, 0u);
  WaitFor([this, baseline] { return FpsDropped() > baseline; },
          std::chrono::milliseconds(300));
  EXPECT_EQ(FpsDropped(), baseline);
}

// A video track switch throws the VideoRendererImpl away and builds a new one,
// so every setting that lives inside it has to be re-installed. Until
// ApplyVideoConfig ran at both construction sites, the cap -- and the filter
// graph, and the decoder preference -- were silently dropped on switch. This is
// the assertion that would have caught it.
TEST_F(MaxFpsOptionTest, TheCapSurvivesAVideoTrackSwitch) {
  CreateRenderer(/*max_fps=*/kCap, /*max_frame_drop=*/0);
  AddVideoStream();
  Initialize();
  ASSERT_TRUE(StartAndPresentOneFrame());
  ASSERT_TRUE(WaitFor([this] { return FpsDropped() > 0; }));

  // A DIFFERENT stream, so this is a real switch and not the same-track no-op.
  DemuxerStream* second = AddVideoStream();
  ASSERT_NE(second, nullptr);
  bool switch_done = false;
  renderer_->OnTracksChanged(
      DemuxerStreamType::kVideo, second,
      base::BindOnce([](bool* flag) { *flag = true; }, &switch_done));
  ASSERT_TRUE(WaitFor([&switch_done] { return switch_done; }))
      << "the video track switch never completed";

  // The replacement renderer is a new object with a new compositor; the cap
  // has to have been installed on it again.
  VideoRendererImpl* video = renderer_->video_renderer();
  ASSERT_NE(video, nullptr);
  EXPECT_EQ(video->max_fps(), kCap);

  // And it has to still be DOING something, not merely hold the right number.
  const uint64_t before = FpsDropped();
  ASSERT_TRUE(WaitFor([this, before] { return FpsDropped() > before; }))
      << "the cap value survived the switch but stopped capping";
}

// "vn" / "an". These DO work -- PlayerImpl passes config.video.disabled and
// config.audio.disabled into RendererImpl::Deps, and Initialize() turns them
// into a null stream -- but nothing asserted it, which is how a two-line
// deletion of those assignments survived a green 528-test run while this file
// was being written.
//
// Both legs are disabled here, so the pipeline reports the missing-streams
// status it reports for a resource with no streams at all (the same status
// MissingStreamsAreReportedNotIgnored asserts): a video-disabled stream is
// indistinguishable from an absent one, which is the point of the flag. Drop
// the assignment and this becomes kOk with a live video renderer.
TEST_F(MaxFpsOptionTest, VideoDisabledLeavesNoVideoRenderer) {
  renderer_ = std::make_unique<RendererImpl>(
      MakeDeps(/*max_fps=*/kCap, /*max_frame_drop=*/0,
               /*video_disabled=*/true, /*audio_disabled=*/true));
  AddVideoStream();
  Initialize();
  ASSERT_TRUE(WaitFor([this] { return init_done_; }));
  EXPECT_EQ(init_status_, PipelineStatus::kMissingDemuxerStreams);

  // The stream is right there in the resource; the flag is the only reason
  // there is no renderer for it.
  EXPECT_NE(resource_.GetStream(DemuxerStreamType::kVideo), nullptr);
  EXPECT_EQ(renderer_->video_renderer(), nullptr);
}

// The control for the test above, and the reason it is not vacuous: the same
// resource with the flag off DOES produce a video renderer.
TEST_F(MaxFpsOptionTest, VideoEnabledProducesAVideoRenderer) {
  CreateRenderer(/*max_fps=*/kCap, /*max_frame_drop=*/0);
  AddVideoStream();
  Initialize();
  ASSERT_TRUE(WaitFor([this] { return init_done_; }));
  EXPECT_EQ(init_status_, PipelineStatus::kOk);

  EXPECT_NE(resource_.GetStream(DemuxerStreamType::kVideo), nullptr);
  EXPECT_NE(renderer_->video_renderer(), nullptr);
}

}  // namespace
}  // namespace avbase::media
