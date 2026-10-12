// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.

// Where a "vf" filter graph is refused, and why that is a decision rather
// than a failure.
//
// VideoRendererImpl builds the graph lazily, on the first decoded frame,
// because it needs the real geometry. It also needs something the geometry
// does not say: whether the frames have CPU planes at all. A hardware-decoded
// frame does not -- VideoFrame::visible_data() returns an empty span for
// kStorageOpaque -- so a CPU graph handed one would be fed null plane
// pointers and reject every frame.
//
// The renderer therefore refuses the graph and says so, rather than dragging
// the pixels back to the CPU, which would quietly undo zero-copy: the most
// expensive way to produce a picture that never needed to move.
//
// The assertion that matters is that the picture keeps being SHOWN in both
// cases. The bug this replaces froze it instead, and surfaced nothing.

#include "media/filters/video_renderer_impl.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/video_frame.h"
#include "media/filters/legacy/video_frame_compositor.h"
#include "media/filters/video_filter_stage.h"
#include "tests/support/fake_decoder_factories.h"
#include "tests/support/fake_demuxer_stream.h"
#include "tests/support/fake_renderer_sinks.h"

namespace avbase::media {
namespace {

// One 30 fps interval, the cadence the fake decoder's frames are stamped with.
constexpr base::TimeDelta kFrameInterval = base::Microseconds(33333);

// Far enough ahead that every scripted frame is due; the compositor compares
// frame timestamps against the master clock, and a "not yet due" clock would
// turn every presentation assertion below into a timing test.
constexpr base::TimeDelta kMasterClock = base::Seconds(10);

// Carried by every frame the stage hands back, and by no decoded frame, so
// "the graph ran" is one comparison rather than a guess at how it transformed
// the pixels.
constexpr int32_t kFilteredSerial = 0x46494c54;  // 'FILT'

struct StageCounters {
  int constructed{0};
  int init_calls{0};
  int process_calls{0};
  bool init_result{true};
  // Set false to make the stage fail on a frame it could otherwise read, i.e.
  // to model a broken graph rather than an unsupported frame.
  bool process_result{true};
  // Fail the FIRST Process() call only. This is the only way to see from
  // outside whether a failed stage was dropped or kept: one that recovers
  // would filter the next frame if it were still around, so "no frame was
  // ever filtered" is the observable form of "it was dropped".
  bool fail_first_process{false};
  std::string graph;
  // The geometry each build saw, in build order. A rebuild is only visible
  // here as a second entry, and the SIZES are what make it mean something: a
  // stage rebuilt against unchanged geometry is a thrash, which is a
  // different bug from a stage that never noticed the change.
  std::vector<Size> init_sizes;
};

// Stands in for the FFmpeg stage: it does no pixel work, and instead answers
// the two questions a test actually has -- was a graph built, and did the
// frames reach it.
class StampingFilterStage final : public VideoFilterStage {
 public:
  explicit StampingFilterStage(StageCounters* counters) : counters_(counters) {
    ++counters_->constructed;
  }

  bool Initialize(const std::string& graph, VideoFormat /*format*/,
                  const Size& coded_size) override {
    ++counters_->init_calls;
    counters_->graph = graph;
    counters_->init_sizes.push_back(coded_size);
    coded_size_ = coded_size;
    return counters_->init_result;
  }

  bool Process(base::scoped_refptr<VideoFrame> in,
               base::scoped_refptr<VideoFrame>* out) override {
    ++counters_->process_calls;
    *out = nullptr;
    if (!in) {
      return true;
    }
    if (counters_->fail_first_process && counters_->process_calls == 1) {
      return false;  // a transient failure the stage would recover from
    }
    // What a real CPU graph must do (and what FFmpegVideoFilter now does):
    // refuse a frame whose planes it cannot read.
    if (!in->IsMappable()) {
      return false;
    }
    if (!counters_->process_result) {
      return false;
    }
    *out = VideoFrame::CreateBlackFrame(
        in->format(), in->coded_size(), in->natural_size(), Rational{1, 1},
        in->timestamp(), in->duration(), kFilteredSerial);
    return true;
  }

  const Size& CodedSize() const override { return coded_size_; }

 private:
  StageCounters* const counters_;
  Size coded_size_;
};

class VideoRendererFilterStageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    runner_ = env_.GetMainThreadTaskRunnerRef();
    stream_ = std::make_unique<test::FakeDemuxerStream>(
        DemuxerStreamType::kVideo, test::MakeValidAudioConfig(),
        test::MakeValidVideoConfig());
  }

  void ScriptBuffers(int buffers) {
    for (int i = 0; i < buffers; ++i) {
      stream_->AppendBuffer(
          test::MakeDataBuffer(DemuxerStreamType::kVideo, stream_->serial()));
    }
  }

  // |graph| empty means "no filter requested", which is the state the two
  // refusal cases are compared against.
  void CreateRenderer(std::string graph, bool with_factory) {
    std::vector<base::scoped_refptr<VideoDecoderFactory>> factories;
    factory_ = base::MakeRefCounted<test::FakeVideoDecoderFactory>(behaviour_);
    factories.push_back(factory_);
    renderer_ = std::make_unique<VideoRendererImpl>(
        runner_, std::move(factories), env_.GetTickClock(),
        VideoFrameCompositor::Thresholds());
    VideoFilterStageFactory filter_factory;
    if (with_factory) {
      filter_factory = base::BindRepeating(
          [](StageCounters* counters) -> std::unique_ptr<VideoFilterStage> {
            return std::make_unique<StampingFilterStage>(counters);
          },
          &counters_);
    }
    renderer_->set_filter_graph(std::move(graph), std::move(filter_factory));
    renderer_->set_ended_cb(base::BindRepeating(
        &VideoRendererFilterStageTest::OnEnded, base::Unretained(this)));
    auto sink = std::make_unique<test::FakeVideoSink>();
    sink_ = sink.get();
    renderer_->Initialize(
        stream_.get(), std::move(sink),
        base::BindOnce(&VideoRendererFilterStageTest::OnInitialized,
                       base::Unretained(this)));
    env_.RunUntilIdle();
  }

  void OnInitialized(PipelineStatus status) {
    init_status_ = status;
    init_done_ = true;
  }

  void OnEnded() { ++ended_calls_; }

  void StartPlaying(int32_t serial) {
    renderer_->StartPlayingFrom(base::TimeDelta());
    renderer_->SetMasterClock(kMasterClock, serial, true);
    env_.RunUntilIdle();
  }

  void PresentFor(int intervals) {
    for (int i = 0; i < intervals; ++i) {
      env_.FastForwardBy(kFrameInterval);
      sink_->PullFrames(1);
      env_.RunUntilIdle();
    }
  }

  int FramesWithSerial(int32_t serial) const {
    int count = 0;
    for (const auto& frame : sink_->frames()) {
      if (frame->serial() == serial) {
        ++count;
      }
    }
    return count;
  }

  base::test::TaskEnvironment env_;
  base::scoped_refptr<base::SequencedTaskRunner> runner_;
  test::FakeDecoderBehaviour behaviour_;
  base::scoped_refptr<test::FakeVideoDecoderFactory> factory_;
  std::unique_ptr<test::FakeDemuxerStream> stream_;
  std::unique_ptr<VideoRendererImpl> renderer_;
  test::FakeVideoSink* sink_ = nullptr;
  StageCounters counters_;
  bool init_done_ = false;
  PipelineStatus init_status_{PipelineStatus::kOk};
  int ended_calls_ = 0;
};

// The case the refusal exists for. The decoder hands the renderer frames with
// no CPU planes, so the graph is never built -- and crucially the frame is
// still handed to the display. Returning early here is what the old code did,
// and it produced a frozen picture with no error: the one outcome a refusal
// must not have.
TEST_F(VideoRendererFilterStageTest, HardwareFramesRefuseTheGraphButStillShow) {
  behaviour_.hardware_frames = true;
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer(/*graph=*/"null", /*with_factory=*/true);
  ASSERT_TRUE(init_done_);
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  StartPlaying(/*serial=*/0);
  PresentFor(/*intervals=*/4);

  EXPECT_EQ(0, counters_.constructed);
  EXPECT_EQ(VideoFilterState::kRefusedHardwareFrame, renderer_->filter_state());
  EXPECT_FALSE(sink_->frames().empty());
  // The decoder's serial, not the filter's: nothing was filtered.
  EXPECT_GT(FramesWithSerial(0), 0);
  EXPECT_EQ(0, FramesWithSerial(kFilteredSerial));
}

// The refusal must be about the frames, not a blanket no. A mappable frame runs
// the graph and the graph's output is what gets shown.
TEST_F(VideoRendererFilterStageTest, MappableFramesRunTheGraph) {
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer(/*graph=*/"null", /*with_factory=*/true);
  ASSERT_TRUE(init_done_);
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  StartPlaying(/*serial=*/0);
  PresentFor(/*intervals=*/4);

  EXPECT_EQ(1, counters_.constructed);
  EXPECT_EQ("null", counters_.graph);
  EXPECT_EQ(VideoFilterState::kRunning, renderer_->filter_state());
  EXPECT_FALSE(sink_->frames().empty());
  EXPECT_GT(FramesWithSerial(kFilteredSerial), 0);
  EXPECT_EQ(0, FramesWithSerial(0));
}

// A graph requested on a build with no stage factory (no-ffmpeg) says so, and
// does not silently swallow the picture.
TEST_F(VideoRendererFilterStageTest, GraphWithoutAFactoryIsRefused) {
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer(/*graph=*/"null", /*with_factory=*/false);
  ASSERT_TRUE(init_done_);
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  StartPlaying(/*serial=*/0);
  PresentFor(/*intervals=*/4);

  EXPECT_EQ(0, counters_.constructed);
  EXPECT_EQ(VideoFilterState::kRefusedNoFactory, renderer_->filter_state());
  EXPECT_FALSE(sink_->frames().empty());
  EXPECT_GT(FramesWithSerial(0), 0);
}

// A stage that fails on a frame it CAN read is broken, not merely unsupported,
// so it gets dropped. The picture keeps flowing: this is the exact shape that
// used to freeze -- the failure return did not repost the pump, so the video
// leg stopped dead and reported nothing.
TEST_F(VideoRendererFilterStageTest, AFailingStageIsDroppedAndFramesFlow) {
  counters_.process_result = false;
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer(/*graph=*/"null", /*with_factory=*/true);
  ASSERT_TRUE(init_done_);
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  StartPlaying(/*serial=*/0);
  PresentFor(/*intervals=*/4);

  EXPECT_EQ(VideoFilterState::kRefusedGraphFailed, renderer_->filter_state());
  // The frame handed to the failing stage is gone with it, but every frame
  // after it still reaches the display.
  EXPECT_FALSE(sink_->frames().empty());
  EXPECT_GT(FramesWithSerial(0), 0);
}

// A stage that fails once is dropped FOR GOOD rather than retried. The
// assertion is the call count: a runner that kept the stage would hand it the
// next frame, which this stage would have filtered successfully.
//
// Worth pinning because it is the difference between failing open once and
// attempting a doomed graph on every frame for the rest of the stream -- and
// because, with a stage that never recovers, dropping and keeping produce
// byte-identical output and no test can tell them apart.
TEST_F(VideoRendererFilterStageTest, AStageThatFailsOnceIsNeverRetried) {
  counters_.fail_first_process = true;
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer(/*graph=*/"null", /*with_factory=*/true);
  ASSERT_TRUE(init_done_);
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  StartPlaying(/*serial=*/0);
  PresentFor(/*intervals=*/4);

  EXPECT_EQ(1, counters_.constructed);
  EXPECT_EQ(1, counters_.process_calls);  // never handed another frame
  EXPECT_EQ(VideoFilterState::kRefusedGraphFailed, renderer_->filter_state());
  EXPECT_FALSE(sink_->frames().empty());
  EXPECT_GT(FramesWithSerial(0), 0);
  EXPECT_EQ(0, FramesWithSerial(kFilteredSerial));
}

// A mid-stream resolution change (Δ3). A filter graph is built against fixed
// geometry and cannot absorb one in place, so the runner drops the stage and
// rebuilds it against the new size. What must NOT happen is the other reading
// of "rebuilt": the frames that arrive in between have to keep reaching the
// display, or a resolution change looks like a stall.
TEST_F(VideoRendererFilterStageTest, GeometryChangeRebuildsTheStage) {
  behaviour_.geometry_change_after = 2;
  ScriptBuffers(/*buffers=*/2);  // 4 frames: 320x240, 320x240, 640x480, 640x480
  CreateRenderer(/*graph=*/"null", /*with_factory=*/true);
  ASSERT_TRUE(init_done_);
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  StartPlaying(/*serial=*/0);
  PresentFor(/*intervals=*/4);

  ASSERT_EQ(2, counters_.constructed);
  ASSERT_EQ(2u, counters_.init_sizes.size());
  EXPECT_EQ((Size{320, 240}), counters_.init_sizes[0]);
  EXPECT_EQ((Size{640, 480}), counters_.init_sizes[1]);
  EXPECT_EQ(VideoFilterState::kRunning, renderer_->filter_state());
  // Both geometries were filtered, not just the second one.
  EXPECT_GT(FramesWithSerial(kFilteredSerial), 0);
}

// No graph requested: the ordinary path, and the state everything else is a
// deviation from.
TEST_F(VideoRendererFilterStageTest, NoGraphRequestedLeavesTheStateOff) {
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer(/*graph=*/"", /*with_factory=*/true);
  ASSERT_TRUE(init_done_);
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  StartPlaying(/*serial=*/0);
  PresentFor(/*intervals=*/4);

  EXPECT_EQ(0, counters_.constructed);
  EXPECT_EQ(VideoFilterState::kOff, renderer_->filter_state());
  EXPECT_FALSE(sink_->frames().empty());
}

}  // namespace
}  // namespace avbase::media
