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
#include "base/functional/callback_helpers.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/decoder_buffer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/media_error.h"
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
      stream_->AppendBuffer(
          test::MakeDataBuffer(DemuxerStreamType::kVideo, stream_->serial()));
    }
  }

  void CreateRenderer() {
    std::vector<base::scoped_refptr<VideoDecoderFactory>> factories;
    factory_ = base::MakeRefCounted<test::FakeVideoDecoderFactory>(behaviour_);
    factories.push_back(factory_);
    renderer_ = std::make_unique<VideoRendererImpl>(
        runner_, std::move(factories), env_.GetTickClock(),
        VideoFrameCompositor::Thresholds());
    renderer_->set_ended_cb(base::BindRepeating(&VideoRendererImplTest::OnEnded,
                                                base::Unretained(this)));
    auto sink = std::make_unique<test::FakeVideoSink>();
    sink_ = sink.get();
    renderer_->Initialize(stream_.get(), std::move(sink),
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
  renderer_->Flush(/*serial=*/1,
                   base::BindOnce([](bool* done) { *done = true; },
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

// ---------------------------------------------------------------------------
// decoder_preference (docs/12 §2.3). These four are the regression anchor for
// the wiring that made config.video.decoder_preference mean anything: before
// it, the preference reached no code at all on the playback path -- the
// factories were tried in injection order whatever the config said, so
// kHardwareOnly ("fail instead of falling back") silently fell back.
// ---------------------------------------------------------------------------

// Builds a renderer over |factories| in the given order, optionally with a
// preference set, and returns after Initialize() has settled.
class PreferenceTest : public VideoRendererImplTest {
 protected:
  void CreateWithPreference(
      std::vector<base::scoped_refptr<test::FakeVideoDecoderFactory>> list,
      bool set_preference, DecoderPreference preference, HwCodecMask mask) {
    std::vector<base::scoped_refptr<VideoDecoderFactory>> factories;
    for (auto& f : list) {
      factories.push_back(f);
    }
    renderer_ = std::make_unique<VideoRendererImpl>(
        runner_, std::move(factories), env_.GetTickClock(),
        VideoFrameCompositor::Thresholds());
    if (set_preference) {
      renderer_->set_decoder_preference(preference, mask);
    }
    auto sink = std::make_unique<test::FakeVideoSink>();
    sink_ = sink.get();
    renderer_->Initialize(
        stream_.get(), std::move(sink),
        base::BindOnce(&PreferenceTest::OnInitialized, base::Unretained(this)));
    env_.RunUntilIdle();
  }

  void OnInitialized(PipelineStatus status) {
    init_status_ = status;
    init_done_ = true;
  }
};

// The headline: a software decoder is INJECTED FIRST, and kHardwareOnly must
// still not use it. Against the pre-fix code the first injected factory won,
// which is exactly the "宁败不回退" (better to fail than fall back) promise
// being broken.
TEST_F(PreferenceTest, HardwareOnlySkipsAnInjectedSoftwareDecoder) {
  auto software = base::MakeRefCounted<test::FakeVideoDecoderFactory>(
      behaviour_, "SoftwareFirst");
  auto hardware = base::MakeRefCounted<test::FakeVideoDecoderFactory>(
      behaviour_, "HardwareSecond");
  hardware->set_hardware(true);
  hardware->set_priority(10);

  // Injected software-first on purpose: the preference, not the order, must
  // decide.
  CreateWithPreference({software, hardware}, /*set_preference=*/true,
                       DecoderPreference::kHardwareOnly,
                       static_cast<HwCodecMask>(HwCodecFlag::kAll));

  ASSERT_TRUE(init_done_);
  EXPECT_EQ(init_status_, PipelineStatus::kOk);
  EXPECT_EQ(software->create_calls(), 0)
      << "kHardwareOnly used a software decoder";
  EXPECT_EQ(hardware->create_calls(), 1);
}

// The mirror: kSoftware must skip an injected hardware decoder. Without this
// the two preferences could be satisfied by the same code path.
TEST_F(PreferenceTest, SoftwareSkipsAnInjectedHardwareDecoder) {
  auto hardware = base::MakeRefCounted<test::FakeVideoDecoderFactory>(
      behaviour_, "HardwareFirst");
  hardware->set_hardware(true);
  auto software = base::MakeRefCounted<test::FakeVideoDecoderFactory>(
      behaviour_, "SoftwareSecond");

  CreateWithPreference({hardware, software}, /*set_preference=*/true,
                       DecoderPreference::kSoftware,
                       static_cast<HwCodecMask>(HwCodecFlag::kAll));

  ASSERT_TRUE(init_done_);
  EXPECT_EQ(init_status_, PipelineStatus::kOk);
  EXPECT_EQ(hardware->create_calls(), 0);
  EXPECT_EQ(software->create_calls(), 1);
}

// kHardwareOnly with NO hardware candidate must FAIL, not quietly use the
// software one. This is the assertion that distinguishes "honoured the
// preference" from "reordered the list and hoped".
TEST_F(PreferenceTest, HardwareOnlyFailsWhenOnlySoftwareExists) {
  auto software = base::MakeRefCounted<test::FakeVideoDecoderFactory>(
      behaviour_, "OnlySoftware");
  CreateWithPreference({software}, /*set_preference=*/true,
                       DecoderPreference::kHardwareOnly,
                       static_cast<HwCodecMask>(HwCodecFlag::kAll));

  ASSERT_TRUE(init_done_) << "no initialization callback at all";
  EXPECT_EQ(init_status_, PipelineStatus::kVideoRendererInitializationError);
  EXPECT_EQ(software->create_calls(), 0);
}

// kAuto keeps the fallback: hardware is preferred, and when it declines the
// software path still runs. The preference reorders; it does not amputate.
TEST_F(PreferenceTest, AutoFallsBackToSoftwareWhenHardwareDeclines) {
  auto hardware = base::MakeRefCounted<test::FakeVideoDecoderFactory>(
      behaviour_, "HardwareDeclines");
  hardware->set_hardware(true);
  hardware->set_priority(10);
  hardware->set_declines(true);
  auto software = base::MakeRefCounted<test::FakeVideoDecoderFactory>(
      behaviour_, "SoftwareFallback");

  CreateWithPreference({software, hardware}, /*set_preference=*/true,
                       DecoderPreference::kAuto,
                       static_cast<HwCodecMask>(HwCodecFlag::kAll));

  ASSERT_TRUE(init_done_);
  EXPECT_EQ(init_status_, PipelineStatus::kOk)
      << "kAuto must keep the software fallback (Δ12)";
  EXPECT_EQ(software->create_calls(), 1);
}

// hw_codecs narrows the hardware path at the codec level: a hardware decoder
// that is otherwise eligible is skipped when its codec is not enabled, and
// under kHardwareOnly that means failing rather than using software.
TEST_F(PreferenceTest, HardwareOnlyRespectsTheCodecMask) {
  auto hardware = base::MakeRefCounted<test::FakeVideoDecoderFactory>(
      behaviour_, "AvcHardware");
  hardware->set_hardware(true);
  auto software = base::MakeRefCounted<test::FakeVideoDecoderFactory>(
      behaviour_, "SoftwareFallback");

  // The fake stream is H.264 (MakeValidVideoConfig); enable only HEVC.
  CreateWithPreference({software, hardware}, /*set_preference=*/true,
                       DecoderPreference::kHardwareOnly,
                       static_cast<HwCodecMask>(HwCodecFlag::kHevc));

  ASSERT_TRUE(init_done_);
  EXPECT_EQ(init_status_, PipelineStatus::kVideoRendererInitializationError);
  EXPECT_EQ(hardware->create_calls(), 0)
      << "a codec outside config.video.hw_codecs reached the hardware path";
  EXPECT_EQ(software->create_calls(), 0);
}

// A host that injected its own list and set NO preference keeps that list
// verbatim. Re-ranking an explicit host decision would be the base overruling
// a caller that has more context than it does.
TEST_F(PreferenceTest, NoPreferenceLeavesTheInjectedOrderAlone) {
  auto first = base::MakeRefCounted<test::FakeVideoDecoderFactory>(
      behaviour_, "InjectedFirst");
  first->set_hardware(true);
  auto second = base::MakeRefCounted<test::FakeVideoDecoderFactory>(
      behaviour_, "InjectedSecond");

  CreateWithPreference({first, second}, /*set_preference=*/false,
                       DecoderPreference::kAuto,
                       static_cast<HwCodecMask>(HwCodecFlag::kAll));

  ASSERT_TRUE(init_done_);
  EXPECT_EQ(init_status_, PipelineStatus::kOk);
  EXPECT_EQ(first->create_calls(), 1)
      << "an unconfigured preference must not re-rank the host's own list";
  EXPECT_EQ(second->create_calls(), 0);
}

// ---------------------------------------------------------------------------
// StopAndDrainForTeardown, the piece video track switching needs and did not
// have (docs/12 4.1). The audio side has had one since Phase 4; without the
// video counterpart the handover has no safe way to retire the old renderer.
// ---------------------------------------------------------------------------

// The closure runs, and it runs on the renderer's own sequence. Both halves
// matter: the first is what lets the caller delete the object, the second is
// what makes deleting it there safe.
TEST_F(VideoRendererImplTest, StopAndDrainRunsItsClosureOnTheRenderSequence) {
  CreateRenderer();
  ASSERT_TRUE(init_done_);
  StartPlaying(0);

  bool ran = false;
  renderer_->StopAndDrainForTeardown(
      base::BindOnce([](bool* ran) { *ran = true; }, base::Unretained(&ran)));
  env_.RunUntilIdle();

  EXPECT_TRUE(ran)
      << "the quiescence closure never ran, so a handover would have nothing "
         "to wait on before deleting the renderer";
}

// The sink is stopped BEFORE the drain, which is the ordering that makes the
// drain safe: after Stop() returns, the sink guarantees no further Render(),
// so nothing can call back into a half-destroyed renderer.
TEST_F(VideoRendererImplTest, StopAndDrainStopsTheSinkFirst) {
  CreateRenderer();
  ASSERT_TRUE(init_done_);
  StartPlaying(0);
  ASSERT_EQ(sink_->stop_count(), 0);

  renderer_->StopAndDrainForTeardown(base::OnceClosure());
  env_.RunUntilIdle();

  // The sink's stop_count is the whole safety argument: once it has stopped,
  // nothing can call Render() back into a renderer that is about to be
  // deleted, so the drain below it is sound.
  EXPECT_EQ(sink_->stop_count(), 1);
}

// A renderer that was never initialized has no demuxer stream to flush, and
// must still run its closure -- a handover that assumed otherwise would hang
// waiting for a drain that has nothing to drain.
TEST_F(VideoRendererImplTest, StopAndDrainOnAnUninitializedRendererStillRuns) {
  std::vector<base::scoped_refptr<VideoDecoderFactory>> factories;
  factory_ = base::MakeRefCounted<test::FakeVideoDecoderFactory>(behaviour_);
  factories.push_back(factory_);
  renderer_ = std::make_unique<VideoRendererImpl>(
      runner_, std::move(factories), env_.GetTickClock(),
      VideoFrameCompositor::Thresholds());
  // Deliberately NOT Initialize()d.

  bool ran = false;
  renderer_->StopAndDrainForTeardown(
      base::BindOnce([](bool* ran) { *ran = true; }, base::Unretained(&ran)));
  env_.RunUntilIdle();

  EXPECT_TRUE(ran)
      << "an uninitialized renderer skipped its closure; a caller tearing one "
         "down would wait forever";
}

// THE ONE THAT BITES. The three above pass even if the drain is replaced by a
// bare Stop(), because they only observe the closure and the sink -- both of
// which a Stop-only implementation also does. This one observes the DRAIN.
//
// The fake decoder defers its Decode reply (defer_decode), so a read really is
// in flight when the teardown starts. The contract is that Flush completes that
// read INLINE, with kDecodingAborted, before the quiescence closure runs --
// which is the difference between "the renderer stopped" and "no task naming it
// can still be created". With the drain removed, the deferred reply is still
// sitting in the decoder when the closure fires, and this fails.
TEST_F(VideoRendererImplTest, StopAndDrainCompletesTheInFlightReadInline) {
  behaviour_.defer_decode = true;
  CreateRenderer();
  ASSERT_TRUE(init_done_);
  ScriptBuffers(/*buffers=*/4);
  StartPlaying(/*serial=*/0);
  env_.RunUntilIdle();

  // A decode is now parked in the fake, unreplied.
  ASSERT_NE(factory_->last_decoder(), nullptr);
  ASSERT_GT(factory_->last_decoder()->deferred_count(), 0u)
      << "no decode was in flight, so the drain would have nothing to do and "
         "this case would be vacuous";

  renderer_->StopAndDrainForTeardown(base::BindOnce(base::DoNothing()));
  env_.RunUntilIdle();

  // The whole point: after the drain, the decoder holds no unreplied decode.
  // Anything still here would run against a deleted renderer.
  EXPECT_EQ(factory_->last_decoder()->deferred_count(), 0u)
      << "the teardown returned with a decode still in flight -- deleting the "
         "renderer now would leave that callback pointing at freed memory";
}

// H2: a terminal decode error must reach the pipeline through SetErrorCB.
// The old code faked a natural end of stream here (ended_ + SetEndOfStream +
// ReportEndedOnce), so a dead video leg showed the UI "played to completion"
// instead of an error.
TEST_F(VideoRendererImplTest, FatalDecodeErrorReachesThePipeline) {
  behaviour_.decode_fails = true;
  // DecoderStream only gives up after kMaxConsecutiveDecodeErrors (20)
  // consecutive failures, so script enough buffers for the fallback chain to
  // exhaust itself and surface the status.
  ScriptBuffers(/*buffers=*/64);
  CreateRenderer();
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  MediaError reported;
  renderer_->SetErrorCB(base::BindRepeating(
      [](MediaError* sink, MediaError e) { *sink = std::move(e); }, &reported));
  StartPlaying(/*serial=*/0);
  env_.RunUntilIdle();

  EXPECT_EQ(reported.code(), ErrorCode::kDecodeFailed)
      << "a terminal decode error was logged but never reported";
  // The old behaviour's tell: the leg pretended it had drained naturally.
  EXPECT_EQ(ended_calls_, 0)
      << "a terminal decode error must not be reported as end of stream";
}

}  // namespace
}  // namespace avbase::media
