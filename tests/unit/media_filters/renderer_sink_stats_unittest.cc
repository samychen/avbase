// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// What the DISPLAY layer counts, and whether it reaches the host.
//
// WHY THIS FILE EXISTS. VideoSinkStats carried frames_dropped,
// submit_failures and measured_refresh_hz, and none of the three was readable
// by anybody: PipelineStatistics only ever read `.render` out of the sink's
// snapshot, so a host could not tell "playing, but nothing is on screen" from
// "playing fine", and a display path that was failing to present looked
// exactly like a video with nothing to show. The numbers were counted for
// rounds and reported nowhere.
//
// The assertions are deliberately made against the SINK's own reading rather
// than against a number typed into the test: what has to hold is that the
// value the sink computed is the value that arrived, so a test that fabricated
// its own counter could not fail if the plumbing broke.
//
// WHAT IS NOT CLAIMED. Multi-leg summation (a second source's refused frames
// land in the same field) is not exercised here -- renderer_multi_source_
// unittest.cc owns the two-leg graph, and this file would have to rebuild it
// to assert one addition.

#include "media/base/renderer_client.h"

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
#include "media/base/decoder_buffer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/pipeline_status.h"
#include "media/base/video_decoder_factory.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/renderer_impl.h"
#include "media/filters/video_renderer_impl.h"
#include "tests/support/fake_decoder_factories.h"
#include "tests/support/fake_demuxer_stream.h"
#include "tests/support/fake_renderer_sinks.h"
#include "tests/support/mock_renderer_client.h"

namespace avbase::media {
namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(5);

class SinkStatsReportingTest : public ::testing::Test {
 protected:
  SinkStatsReportingTest()
      : env_(base::test::TaskEnvironment::TimeSource::kRealTime),
        video_thread_("avbase-test-S3"),
        audio_thread_("avbase-test-S4") {}

  void SetUp() override {
    runner_ = env_.GetMainThreadTaskRunnerRef();
    ASSERT_TRUE(video_thread_.Start());
    ASSERT_TRUE(audio_thread_.Start());
  }

  void TearDown() override {
    if (renderer_) {
      env_.RunUntilIdle();
      renderer_.reset();
    }
    video_thread_.Stop();
    audio_thread_.Stop();
  }

  // Video only: the audio leg would add a second clock that has to be kept
  // running for the video one to matter, and no counter under test lives there.
  RendererImpl::Deps MakeDeps() {
    RendererImpl::Deps deps;
    deps.media_task_runner = runner_;
    deps.tick_clock = env_.GetTickClock();
    deps.audio_disabled = true;
    deps.video_task_runner = video_thread_.task_runner();
    deps.audio_task_runner = audio_thread_.task_runner();
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

  // 400 buffers, two outputs each: comfortably more material than the pulls
  // below consume, so a short stream cannot turn "wait for a frame" into a
  // race against the end of the data.
  void AddVideoStream() {
    auto stream = std::make_unique<test::FakeDemuxerStream>(
        DemuxerStreamType::kVideo, test::MakeValidAudioConfig(),
        test::MakeValidVideoConfig());
    for (int i = 0; i < 400; ++i) {
      stream->AppendBuffer(
          test::MakeDataBuffer(DemuxerStreamType::kVideo, stream->serial()));
    }
    resource_.set_stream(DemuxerStreamType::kVideo, stream.get());
    video_streams_.push_back(std::move(stream));
  }

  void Initialize() {
    renderer_->Initialize(&resource_, &client_, runner_,
                          base::BindOnce(&SinkStatsReportingTest::OnInitialized,
                                         base::Unretained(this)));
  }

  void OnInitialized(PipelineStatus status) {
    init_done_ = true;
    init_status_ = status;
  }

  // Pumps the media sequence and pulls from the sink the way a running display
  // would, until |predicate| holds. Bounded, so a stall fails instead of
  // hanging.
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

  // Brings the renderer to the point where the sink is asked for frames.
  bool StartPulling() {
    renderer_ = std::make_unique<RendererImpl>(MakeDeps());
    AddVideoStream();
    Initialize();
    if (!WaitFor([this] { return init_done_; })) {
      return false;
    }
    EXPECT_EQ(init_status_, PipelineStatus::kOk);
    renderer_->StartPlayingFrom(base::TimeDelta());
    return true;
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

// The sink had nowhere to draw, so it counted drops -- and the host can see
// them. Before this, the count existed inside the sink and stopped there:
// video_frames_dropped stayed 0 and the pipeline looked healthy.
TEST_F(SinkStatsReportingTest, FramesWithNoSurfaceReachTheHostAsSinkDrops) {
  ASSERT_TRUE(StartPulling());
  video_sink_->set_has_output(false);

  ASSERT_TRUE(
      WaitFor([this] { return video_sink_->GetStats().frames_dropped >= 3; }));

  // The sink's own reading is the oracle: the value it computed is the value
  // that has to arrive, not a number this test made up.
  const uint64_t refused = video_sink_->GetStats().frames_dropped;
  const PipelineStatistics stats = renderer_->GetStatistics();
  EXPECT_EQ(stats.video_frames_dropped_by_sink, refused);
  EXPECT_GE(stats.video_frames_dropped_by_sink, 3u);
  // The compositor handed them over, so they are part of what it presented --
  // which is exactly why the two counters may not be added up.
  EXPECT_GE(stats.video_frames_presented, stats.video_frames_dropped_by_sink);
  // Nothing failed to upload; there was simply nowhere to put the result.
  EXPECT_EQ(stats.video_frames_submit_failures, 0u);
}

// The other side of the same field: with a surface, the same pulls are
// presented and the counter stays at zero. Without this case, an implementation
// that reported "frames asked for" as drops would pass the test above.
TEST_F(SinkStatsReportingTest, AFramesWithASurfaceIsNotADrop) {
  ASSERT_TRUE(StartPulling());

  ASSERT_TRUE(WaitFor(
      [this] { return video_sink_->GetStats().frames_presented >= 3; }));

  const PipelineStatistics stats = renderer_->GetStatistics();
  EXPECT_EQ(stats.video_frames_dropped_by_sink, 0u);
  EXPECT_GE(stats.video_frames_presented, 3u);
  EXPECT_EQ(stats.video_frames_presented,
            video_sink_->GetStats().frames_presented);
}

// A frame that reached the display layer and could not be shown is a submit
// failure, and it has its own field: the renderer already reported each one
// upward (OnFrameSubmitFailure) and nothing counted the reports, which is why
// a GPU that lost its context looked like a video with nothing to show.
TEST_F(SinkStatsReportingTest, FramesThatFailToUploadReachTheHostAsFailures) {
  ASSERT_TRUE(StartPulling());
  video_sink_->set_submits_fail(true);

  ASSERT_TRUE(
      WaitFor([this] { return video_sink_->GetStats().submit_failures >= 3; }));

  const PipelineStatistics stats = renderer_->GetStatistics();
  EXPECT_EQ(stats.video_frames_submit_failures,
            video_sink_->GetStats().submit_failures);
  EXPECT_GE(stats.video_frames_submit_failures, 3u);
  // A failed submit is not a drop: this frame had somewhere to go and did not
  // get there. Only one of the two counters may move for one frame.
  EXPECT_EQ(stats.video_frames_dropped_by_sink, 0u);
}

// The measured cadence travels with the rest. The expected value is the one
// the sink reported, not a constant derived from its 16 ms interval here: a
// second copy of that arithmetic in the test would agree with a sink that
// computed it wrongly.
TEST_F(SinkStatsReportingTest, TheMeasuredRefreshRateReachesTheHost) {
  ASSERT_TRUE(StartPulling());
  ASSERT_TRUE(WaitFor(
      [this] { return video_sink_->GetStats().frames_presented >= 1; }));

  const PipelineStatistics stats = renderer_->GetStatistics();
  EXPECT_DOUBLE_EQ(stats.measured_refresh_hz,
                   video_sink_->GetStats().measured_refresh_hz);
  EXPECT_GT(stats.measured_refresh_hz, 0.0);
}

}  // namespace
}  // namespace avbase::media
