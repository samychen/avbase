// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// VideoRendererImpl's half of the renderer contract, with no FFmpeg and one
// thread: the compositor is fed by a scripted decoder, the "display" is a fake
// sink the test pulls by hand, and every presentation is asserted on the frame
// that came back.
//
// The two defects this pins are the ones the tenth round could only see on a
// real file after a seek: a frame from the flushed generation being presented
// (#2 in docs/PROGRESS §(5) -- the NAL corruption after the first-play seek),
// and a paused renderer still handing frames to the display.
//
// Single-threaded on purpose: VideoRendererImpl's destructor only stops its
// sink, so nothing here needs a second thread, and a mock clock makes the
// presentation deadlines exact. RendererImpl's suite is the one that needs real
// threads, because ~RendererImpl waits on its sub-renderers' sequences.

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
#include "media/base/decoder_buffer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/pipeline_status.h"
#include "media/base/video_decoder_factory.h"
#include "media/base/video_frame.h"
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

class VideoRendererImplTest : public ::testing::Test {
 protected:
  void SetUp() override {
    runner_ = env_.GetMainThreadTaskRunnerRef();
    stream_ = std::make_unique<test::FakeDemuxerStream>(
        DemuxerStreamType::kVideo, test::MakeValidAudioConfig(),
        test::MakeValidVideoConfig());
  }

  // Scripts |buffers| decodable buffers at the stream's current serial, then
  // EOS (the fake appends the EOS marker itself on the first empty read).
  void ScriptBuffers(int buffers) {
    for (int i = 0; i < buffers; ++i) {
      stream_->AppendBuffer(test::MakeDataBuffer(DemuxerStreamType::kVideo,
                                                 stream_->serial()));
    }
  }

  void CreateRenderer() {
    std::vector<base::scoped_refptr<VideoDecoderFactory>> factories;
    factory_ = base::MakeRefCounted<test::FakeVideoDecoderFactory>(behaviour_);
    factories.push_back(factory_);
    renderer_ = std::make_unique<VideoRendererImpl>(
        runner_, std::move(factories), env_.GetTickClock(),
        VideoFrameCompositor::Thresholds());
    renderer_->set_ended_cb(base::BindRepeating(
        &VideoRendererImplTest::OnEnded, base::Unretained(this)));
    auto sink = std::make_unique<test::FakeVideoSink>();
    sink_ = sink.get();
    renderer_->Initialize(
        stream_.get(), std::move(sink),
        base::BindOnce(&VideoRendererImplTest::OnInitialized,
                       base::Unretained(this)));
    env_.RunUntilIdle();
  }

  void OnInitialized(PipelineStatus status) {
    init_status_ = status;
    init_done_ = true;
  }

  void OnEnded() { ++ended_calls_; }

  // Starts playback on the compositor's media timeline. SetMasterClock is what
  // RendererImpl pushes every 10 ms on a live pipeline; doing it once is enough
  // because the frames below are all in the first interval.
  void StartPlaying(int32_t serial) {
    renderer_->StartPlayingFrom(base::TimeDelta());
    renderer_->SetMasterClock(kMasterClock, serial, true);
    env_.RunUntilIdle();
  }

  // Drives the display for one interval: advance the mock clock, then let the
  // sink ask the renderer for whatever is due.
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
  bool init_done_ = false;
  PipelineStatus init_status_ = PipelineStatus::kOk;
  int ended_calls_ = 0;
};

// The baseline every other test perturbs: data decodes, frames reach the
// display carrying the stream's serial, and the drained stream reports ended
// exactly once.
TEST_F(VideoRendererImplTest, DecodesPresentsAndEnds) {
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer();
  ASSERT_TRUE(init_done_);
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  StartPlaying(/*serial=*/0);
  PresentFor(/*intervals=*/4);

  EXPECT_EQ(sink_->frames().size(), 2u);
  EXPECT_EQ(FramesWithSerial(0), 2);
  EXPECT_EQ(ended_calls_, 1);
  EXPECT_TRUE(renderer_->ended());
  EXPECT_EQ(renderer_->frames_pending(), 0u);
}

// Pause gates the display, not just the decoder: a paused renderer must not
// hand the compositor's next frame to the sink, because that frame would be
// presented *while the pipeline is paused* -- which on a real display is a
// visible jump the moment audio resumes.
TEST_F(VideoRendererImplTest, PausedRendererPresentsNothingNew) {
  ScriptBuffers(/*buffers=*/2);
  CreateRenderer();
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  StartPlaying(/*serial=*/0);
  PresentFor(/*intervals=*/2);
  const size_t while_playing = sink_->frames().size();
  ASSERT_GT(while_playing, 0u);

  renderer_->SetPaused(true);
  env_.RunUntilIdle();
  PresentFor(/*intervals=*/4);
  EXPECT_EQ(sink_->frames().size(), while_playing)
      << "a paused renderer presented another frame";

  renderer_->SetPaused(false);
  env_.RunUntilIdle();
  PresentFor(/*intervals=*/4);
  EXPECT_GT(sink_->frames().size(), while_playing)
      << "resuming did not restart presentation";
}

// #2 in docs/PROGRESS §(5), the one that showed up as NAL corruption after the
// first seek: Flush() bumps the serial, and nothing from the flushed generation
// may reach the display afterwards. The frame counts are asserted rather than
// just the queue, because "the compositor was flushed" and "no stale frame is
// ever presented" are different claims.
TEST_F(VideoRendererImplTest, FlushDropsFramesFromThePreviousSerial) {
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer();
  ASSERT_EQ(init_status_, PipelineStatus::kOk);
  StartPlaying(/*serial=*/0);
  PresentFor(/*intervals=*/4);
  const int old_serial_frames = FramesWithSerial(0);
  ASSERT_GT(old_serial_frames, 0);

  // The seek: a new generation arrives, and the renderer is flushed for it.
  stream_->set_serial(1);
  ScriptBuffers(/*buffers=*/1);
  bool flush_done = false;
  renderer_->Flush(/*serial=*/1, base::BindOnce(
                                     [](bool* done) { *done = true; },
                                     base::Unretained(&flush_done)));
  env_.RunUntilIdle();

  EXPECT_TRUE(flush_done);
  EXPECT_EQ(renderer_->frames_pending(), 0u)
      << "the compositor kept frames from the flushed generation";
  EXPECT_FALSE(renderer_->ended()) << "Flush clears the ended flag";

  StartPlaying(/*serial=*/1);
  PresentFor(/*intervals=*/4);

  EXPECT_GT(FramesWithSerial(1), 0) << "the new generation never presented";
  EXPECT_EQ(FramesWithSerial(0), old_serial_frames)
      << "a frame from the flushed generation reached the display after the "
         "seek";
}

}  // namespace
}  // namespace avbase::media
