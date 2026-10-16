// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// RendererImpl's multi-source contract: stage 1 (docs/14) gave every source
// its own video chain; stage 2 paces each chain against the master clock
// expressed in that source's own timeline.
//
// WHY THIS FILE EXISTS. Stage 1's refactor keeps single-source behaviour
// byte-identical -- the 357 green media tests prove that and say nothing
// about the second source. "Every source got a decode chain" is observable
// only through the sinks the chains present into, and the factory that
// created those sinks is the witness of the assembly (fake_sink_factories.h
// says the same about the single-source suites). The assertions below
// therefore pair a structural half (counts, identities, factory records)
// with a behavioural half (both chains emit frames, secondary audio is never
// read, ended waits for every leg), because either half alone is a bug in a
// different place: a count that moves without frames is a chain that decodes
// into nothing, and frames without the count is a chain nobody built.
//
// Real threads, bounded one-sided waits: the discipline and the reasons are
// renderer_impl_unittest.cc's header (a fake runner deadlocks in
// ~RendererImpl's DestroyOn). The one exception below is a deliberately
// bounded NEGATIVE wait, where the suite wants a state to NOT arrive.

#include "media/filters/renderer_impl.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

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
#include "media/base/media_error.h"
#include "media/base/pipeline_status.h"
#include "media/base/renderer.h"
#include "media/base/video_decoder_factory.h"
#include "media/base/video_frame.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "tests/support/fake_decoder_factories.h"
#include "tests/support/fake_demuxer_stream.h"
#include "tests/support/fake_renderer_sinks.h"
#include "tests/support/fake_sink_factories.h"
#include "tests/support/mock_renderer_client.h"

namespace avbase::media {
namespace {

// Chunk size for the audio ring, and the ceiling for every wait. Generous
// because it is a ceiling and not a delay: each wait returns as soon as its
// state is reached.
constexpr int kFramesPerBuffer = 256;
constexpr auto kWaitTimeout = std::chrono::seconds(5);

// Distinct serials for the two sources' video streams. The serial rides on
// every decoded frame (the fake decoder copies it from the buffer), so the
// serial of the frames a sink received is a behavioural witness of WHICH
// source's leg that sink belongs to -- which is what turns "the host sink
// went to source 0" from a structural claim into an observable one.
constexpr int32_t kPrimaryVideoSerial = 1000;
constexpr int32_t kSecondaryVideoSerial = 2000;

class RendererMultiSourceTest : public ::testing::Test {
 protected:
  RendererMultiSourceTest()
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

  // The stage-1 shape this suite is about. Source 0 carries video AND audio
  // (the composited layer); source 1 carries video AND audio too -- its audio
  // stream is the "secondary audio is seen but not selected" case (§4.2), and
  // scripting buffers into it is what makes "never read" a meaningful
  // assertion rather than "there was nothing to read".
  void SetTwoSources() {
    for (int source = 0; source < 2; ++source) {
      auto video = std::make_unique<test::FakeDemuxerStream>(
          DemuxerStreamType::kVideo, test::MakeValidAudioConfig(),
          test::MakeValidVideoConfig());
      video->set_stream_index(source * 2);
      video->set_serial(source == 0 ? kPrimaryVideoSerial
                                    : kSecondaryVideoSerial);
      resource_.set_stream(source, DemuxerStreamType::kVideo, video.get());
      video_streams_.push_back(std::move(video));

      auto audio = std::make_unique<test::FakeDemuxerStream>(
          DemuxerStreamType::kAudio, test::MakeValidAudioConfig(),
          test::MakeValidVideoConfig());
      audio->set_stream_index(source * 2 + 1);
      resource_.set_stream(source, DemuxerStreamType::kAudio, audio.get());
      audio_streams_.push_back(std::move(audio));
    }
  }

  // |buffers| worth of scripted data per stream, then EOS (FakeDemuxerStream
  // emits EOS from the read that finds nothing left).
  void ScriptBuffers(int buffers) {
    behaviour_.outputs_per_buffer = 2;
    for (int i = 0; i < buffers; ++i) {
      for (auto& video : video_streams_) {
        video->AppendBuffer(
            test::MakeDataBuffer(DemuxerStreamType::kVideo, video->serial()));
      }
      for (auto& audio : audio_streams_) {
        audio->AppendBuffer(
            test::MakeDataBuffer(DemuxerStreamType::kAudio, audio->serial()));
      }
    }
  }

  // Collapses every stream of both sources onto ONE seek generation. The
  // compositor paces a leg against the master clock only while the clock's
  // serial matches the frame's (the generation guard in
  // media/base/decoder_buffer_queue.h), and the master clock is written from
  // source 0's audio. In the pipeline every queue bumps in lockstep on a
  // seek, so one generation across sources is the real shape; the DISTINCT
  // serials above are the sink-ownership witness, which answers a different
  // question. Call before ScriptBuffers, which stamps the serial into the
  // buffers.
  void CohereGenerations(int32_t serial) {
    for (auto& video : video_streams_) {
      video->set_serial(serial);
    }
    for (auto& audio : audio_streams_) {
      audio->set_serial(serial);
    }
  }

  // Build the Deps, wiring |extra| as the secondary-leg factory. MakeDeps()
  // passes a plain FakeVideoSinkFactory (the suite's default); the stage-3
  // test passes a SourceAwareVideoSinkFactory to observe the source id the
  // renderer requests.
  RendererImpl::Deps
  MakeDepsWith(std::shared_ptr<VideoRendererSinkFactory> extra) {
    RendererImpl::Deps deps;
    deps.media_task_runner = runner_;
    deps.tick_clock = env_.GetTickClock();
    deps.audio_frames_per_buffer = kFramesPerBuffer;
    deps.video_task_runner = video_thread_.task_runner();
    deps.audio_task_runner = audio_thread_.task_runner();
    video_factory_ =
        base::MakeRefCounted<test::FakeVideoDecoderFactory>(behaviour_);
    deps.video_factories.emplace_back(video_factory_);
    audio_factory_ =
        base::MakeRefCounted<test::FakeAudioDecoderFactory>(behaviour_);
    deps.audio_factories.emplace_back(audio_factory_);
    // The host sink: source 0's in every pipeline, which is the claim the
    // sink-policy assertions below hold the renderer to.
    auto sink = std::make_unique<test::FakeVideoSink>();
    host_sink_ = sink.get();
    deps.video_sink = std::move(sink);
    // The secondary legs' only legal destination in stage 1. Recording every
    // Create() is what lets these tests see the assembly.
    deps.extra_video_sink_factory = std::move(extra);
    audio_sink_ = base::MakeRefCounted<test::FakeAudioSink>();
    deps.audio_sink = base::scoped_refptr<AudioRendererSink>(audio_sink_.get());
    av_sync_ = std::make_shared<AvSyncController>(
        AvSyncController::MasterType::kAudio, env_.GetTickClock(),
        AvSyncController::Thresholds());
    deps.av_sync = av_sync_;
    return deps;
  }

  RendererImpl::Deps MakeDeps() {
    extra_factory_ = std::make_shared<test::FakeVideoSinkFactory>();
    return MakeDepsWith(extra_factory_);
  }

  void CreateRenderer() {
    renderer_ = std::make_unique<RendererImpl>(MakeDeps());
  }

  void CreateRendererWith(RendererImpl::Deps deps) {
    renderer_ = std::make_unique<RendererImpl>(std::move(deps));
  }

  void Initialize() {
    renderer_->Initialize(
        &resource_, &client_, runner_,
        base::BindOnce(&RendererMultiSourceTest::OnInitialized,
                       base::Unretained(this)));
  }

  void OnInitialized(PipelineStatus status) {
    ++init_calls_;
    init_status_ = status;
    init_done_ = true;
  }

  // Pumps the media sequence while waiting for |predicate|, pulling the sinks
  // as a display and a device would. Bounded: a stall fails the test instead
  // of hanging it.
  template <typename Predicate>
  bool WaitFor(Predicate predicate,
               std::chrono::milliseconds timeout = kWaitTimeout) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
      env_.RunUntilIdle();
      if (predicate()) {
        return true;
      }
      PumpSinks();
      if (std::chrono::steady_clock::now() - start > timeout) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // The secondary sinks to pump, from whichever extra factory the test
  // wired. The stage-3 test uses a SourceAwareVideoSinkFactory (which is a
  // different type from FakeVideoSinkFactory), so this collapses the two into
  // one list instead of PumpSinks() reaching into a specific member that may
  // be null for the other test shape.
  std::vector<test::FakeVideoSink*> SecondarySinks() const {
    if (source_aware_factory_) {
      return source_aware_factory_->sinks();
    }
    if (extra_factory_) {
      return extra_factory_->sinks();
    }
    return {};
  }

  // One display interval's worth of frames per video sink plus one audio
  // device period. |pump_secondary_| is the one lever a test has over WHICH
  // legs get pulled: with it off, the secondary leg's frames decode but
  // nothing pulls them out of its compositor, so its pending count never
  // reaches zero -- which is what the ended test below needs.
  void PumpSinks() {
    if (host_sink_) {
      host_sink_->PullFrames(4);
    }
    if (pump_secondary_) {
      for (test::FakeVideoSink* sink : SecondarySinks()) {
        sink->PullFrames(4);
      }
    }
    PullAudioPeriod();
  }

  int PullAudioPeriod() {
    if (!audio_bus_) {
      audio_bus_ = AudioBus::Create(2, kFramesPerBuffer);
    }
    const int frames = audio_sink_->PullPeriod(audio_bus_.get());
    if (frames > 0) {
      ++audio_periods_with_data_;
    }
    return frames;
  }

  // Same bounded drain as renderer_impl_unittest.cc's: run what is ready,
  // then destroy. The weak-bound S1 hops make a tick still in flight inert
  // rather than fatal, so this is a backstop that fails loudly, never a wait
  // that is expected to elapse.
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

  // Declaration order matters: the threads and the environment must outlive
  // the renderer they are handed to, and ~RendererImpl blocks on the threads.
  base::test::TaskEnvironment env_;
  base::Thread video_thread_;
  base::Thread audio_thread_;
  base::scoped_refptr<base::SequencedTaskRunner> runner_;
  test::FakeDecoderBehaviour behaviour_;
  base::scoped_refptr<test::FakeVideoDecoderFactory> video_factory_;
  base::scoped_refptr<test::FakeAudioDecoderFactory> audio_factory_;
  std::shared_ptr<AvSyncController> av_sync_;
  test::FakeVideoSink* host_sink_ = nullptr;
  std::shared_ptr<test::FakeVideoSinkFactory> extra_factory_;
  std::shared_ptr<test::SourceAwareVideoSinkFactory> source_aware_factory_;
  base::scoped_refptr<test::FakeAudioSink> audio_sink_;
  test::FakeMediaResource resource_;
  std::vector<std::unique_ptr<test::FakeDemuxerStream>> video_streams_;
  std::vector<std::unique_ptr<test::FakeDemuxerStream>> audio_streams_;
  std::unique_ptr<RendererImpl> renderer_;
  test::FakeRendererClient client_;
  std::unique_ptr<AudioBus> audio_bus_;
  int audio_periods_with_data_ = 0;
  int init_calls_ = 0;
  bool init_done_ = false;
  bool pump_secondary_ = true;
  PipelineStatus init_status_ = PipelineStatus::kOk;
};

// The contract's core: TWO video chains, not one. The structural half --
// video_renderer_count() and the two decoders the shared factory was asked
// for -- and the behavioural half -- both sinks receive frames -- are
// asserted together because either can be right while the other is wrong
// (a count of 2 whose second leg decodes into a sink nobody pulls would pass
// the first and fail the second).
TEST_F(RendererMultiSourceTest, EverySourceGetsItsOwnVideoChain) {
  SetTwoSources();
  ScriptBuffers(/*buffers=*/2);
  CreateRenderer();
  Initialize();

  ASSERT_TRUE(WaitFor([this] { return init_done_; }));
  EXPECT_EQ(init_calls_, 1);
  EXPECT_EQ(init_status_, PipelineStatus::kOk);

  // Structural half. Two legs, and TWO decoders out of the one shared
  // factory -- which is what "a decode chain per source" means, as opposed
  // to a second bookkeeping entry around one chain.
  EXPECT_EQ(renderer_->video_renderer_count(), 2u);
  EXPECT_EQ(video_factory_->create_calls(), 2);
  EXPECT_EQ(video_factory_->counters().init_calls, 2);
  // The audio leg is still exactly one: source 0's.
  EXPECT_EQ(audio_factory_->create_calls(), 1);

  // The sink policy: the host sink went to source 0's leg, and the factory
  // was asked for exactly the one secondary leg. If the policy handed the
  // host sink to a later source -- or built the primary leg out of the
  // factory -- one of these two is where it shows up.
  ASSERT_EQ(extra_factory_->sinks().size(), 1u);
  EXPECT_NE(extra_factory_->sinks()[0], host_sink_);

  // Behavioural half: both chains present, and EACH sink carries its own
  // source's frames. The serials make that second half observable: the host
  // sink holding only primary-serial frames and the factory sink holding
  // only secondary-serial frames is the sink policy doing its job, not two
  // bookkeeping entries around one chain observed twice. A policy that
  // handed the host sink to source 1's leg swaps these two sets.
  renderer_->StartPlayingFrom(base::TimeDelta());
  ASSERT_TRUE(WaitFor([this] { return !host_sink_->frames().empty(); }))
      << "source 0's leg never presented a frame";
  const std::vector<test::FakeVideoSink*> secondary = extra_factory_->sinks();
  ASSERT_EQ(secondary.size(), 1u);
  ASSERT_TRUE(WaitFor([sink = secondary[0]] {
    return !sink->frames().empty();
  })) << "source 1's leg never presented a frame";
  for (const base::scoped_refptr<VideoFrame>& frame : host_sink_->frames()) {
    EXPECT_EQ(frame->serial(), kPrimaryVideoSerial)
        << "the host sink received a frame that is not source 0's";
  }
  for (const base::scoped_refptr<VideoFrame>& frame : secondary[0]->frames()) {
    EXPECT_EQ(frame->serial(), kSecondaryVideoSerial)
        << "the secondary sink received a frame that is not source 1's";
  }
}

// §4.2: secondary audio is SEEN but not selected. The assertion is a zero
// read count on a stream that HAS scripted data -- "read and ignored" and
// "never wired" are different defects and only the count tells them apart,
// because a leg that consumed source 1's audio would also have decoded it
// (audio_factory_->create_calls() above is the other half of that witness).
TEST_F(RendererMultiSourceTest, SecondaryAudioIsSeenButNotSelected) {
  SetTwoSources();
  ScriptBuffers(/*buffers=*/2);
  CreateRenderer();
  Initialize();
  ASSERT_TRUE(WaitFor([this] { return init_done_; }));
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  renderer_->StartPlayingFrom(base::TimeDelta());

  // Wait for the PRIMARY audio leg to be genuinely flowing first: asserting
  // a zero before anything has been read would pass against a pipeline whose
  // audio never started at all.
  ASSERT_TRUE(WaitFor([this] { return audio_streams_[0]->read_count() > 0; }))
      << "source 0's audio leg never read";
  EXPECT_EQ(audio_streams_[1]->read_count(), 0);
  EXPECT_EQ(audio_factory_->create_calls(), 1);
}

// "Ended" now means EVERY leg drained (docs/14 stage 1). The negative wait
// is the assertion: with the secondary leg's sink unpumped, its frames
// decode and its EOS arrives, but nothing pulls them out of the compositor,
// so its pending count never reaches zero -- and the pipeline must not
// report ended while that is true, even though source 0's leg and the audio
// leg have fully drained. A source-0-only ended condition fires here.
TEST_F(RendererMultiSourceTest, EndedWaitsForEveryLegToDrain) {
  SetTwoSources();
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer();
  Initialize();
  ASSERT_TRUE(WaitFor([this] { return init_done_; }));

  pump_secondary_ = false;
  renderer_->StartPlayingFrom(base::TimeDelta());

  // Both drainable legs drain: source 0's frames come out and the audio ring
  // empties. The secondary leg cannot, by construction of this test.
  ASSERT_TRUE(WaitFor([this] { return !host_sink_->frames().empty(); }));
  ASSERT_TRUE(WaitFor([this] { return audio_periods_with_data_ > 0; }));
  EXPECT_FALSE(WaitFor([this] { return client_.Has("ended"); },
                       std::chrono::milliseconds(500)))
      << "ended fired while source 1's leg still held frames";

  // Now the secondary leg drains too, and the same wait succeeds -- once.
  pump_secondary_ = true;
  EXPECT_TRUE(WaitFor([this] { return client_.Has("ended"); }))
      << "no OnEnded after every leg drained";
  EXPECT_EQ(client_.Count("ended"), 1);
  EXPECT_TRUE(renderer_->ended());
  // The drain path must not have taken a failure branch on the way here: an
  // ended AND an error would hide a real defect behind the event being
  // waited for.
  EXPECT_EQ(client_.Count("error"), 0);
}

// The foreign-stream guard, video side. The handover machinery switches
// source 0's alternates: it re-anchors against video_stream_, hands over
// source 0's sink, and re-wires source 0's clock hook. Source 1's stream is
// none of those things, and CompositeDemuxer::SetActiveStream only routes
// source 0 today -- so this is unreachable through the pipeline, which is
// exactly why it needs a direct test: a guard nothing exercises is a guard
// nothing keeps correct.
TEST_F(RendererMultiSourceTest, SwitchingToAnotherSourcesVideoTrackIsRefused) {
  SetTwoSources();
  // Plenty of data: the switch must happen mid-stream, not after EOS, or the
  // kInvalidState branch answers first and the guard is never reached.
  ScriptBuffers(/*buffers=*/40);
  CreateRenderer();
  Initialize();
  ASSERT_TRUE(WaitFor([this] { return init_done_; }));
  renderer_->StartPlayingFrom(base::TimeDelta());
  ASSERT_TRUE(WaitFor([this] { return !host_sink_->frames().empty(); }));

  VideoRendererImpl* before = renderer_->video_renderer();
  bool switch_done = false;
  renderer_->OnTracksChanged(
      DemuxerStreamType::kVideo, video_streams_[1].get(),
      base::BindOnce([](bool* flag) { *flag = true; }, &switch_done));

  // Refused, not crashed and not silently wired into source 0's renderer:
  // the caller's callback still runs (a wait on it must not hang), the error
  // says why, and source 0's renderer is the same object as before.
  ASSERT_TRUE(WaitFor([&switch_done] { return switch_done; }));
  EXPECT_TRUE(client_.Has("error"));
  EXPECT_EQ(client_.last_error().code(), ErrorCode::kNotImplemented);
  EXPECT_EQ(renderer_->video_renderer(), before);

  // ...and source 0's leg keeps rendering after the refusal. A guard that
  // rejected the switch but broke the primary leg would pass every assertion
  // above.
  const size_t frames_before = host_sink_->frames().size();
  EXPECT_TRUE(WaitFor([this, frames_before] {
    return host_sink_->frames().size() > frames_before;
  })) << "source 0's leg stopped presenting after a refused switch";
}

// The same guard on the audio side (§4.2): the audio leg belongs to source
// 0, so source 1's audio stream has no renderer to switch INTO. The
// "untouched" half is the factory count -- a replacement AudioRendererImpl
// would have initialized a second decoder.
TEST_F(RendererMultiSourceTest, SwitchingToAnotherSourcesAudioTrackIsRefused) {
  SetTwoSources();
  ScriptBuffers(/*buffers=*/40);
  CreateRenderer();
  Initialize();
  ASSERT_TRUE(WaitFor([this] { return init_done_; }));
  renderer_->StartPlayingFrom(base::TimeDelta());
  ASSERT_TRUE(WaitFor([this] { return audio_streams_[0]->read_count() > 0; }));

  bool switch_done = false;
  renderer_->OnTracksChanged(
      DemuxerStreamType::kAudio, audio_streams_[1].get(),
      base::BindOnce([](bool* flag) { *flag = true; }, &switch_done));

  ASSERT_TRUE(WaitFor([&switch_done] { return switch_done; }));
  EXPECT_TRUE(client_.Has("error"));
  EXPECT_EQ(client_.last_error().code(), ErrorCode::kNotImplemented);
  EXPECT_EQ(audio_factory_->create_calls(), 1);

  // The primary audio leg keeps flowing.
  const int reads_before = audio_streams_[0]->read_count();
  EXPECT_TRUE(WaitFor([this, reads_before] {
    return audio_streams_[0]->read_count() > reads_before;
  })) << "source 0's audio leg stopped reading after a refused switch";
}

// ---- per-source playback offsets (docs/14 stage 2) -------------------------

// The wiring half: the renderer registers every source with the clock
// controller at Initialize, forwards a stated offset, and paces each leg
// against the master clock minus that leg's offset. The read side
// (VideoRendererImpl::master_clock()) is what makes the last step observable
// without watching frames -- a frame-count assertion could not tell a lost
// offset from a frame that was merely early.
TEST_F(RendererMultiSourceTest, TheOffsetReachesEveryLegsClock) {
  SetTwoSources();
  CohereGenerations(4242);
  ScriptBuffers(/*buffers=*/40);
  CreateRenderer();
  Initialize();
  ASSERT_TRUE(WaitFor([this] { return init_done_; }));
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  // Registered, and defaulted to time-aligned: an unstated offset is zero,
  // not "unknown".
  EXPECT_EQ(av_sync_->source_offset(0), base::TimeDelta());
  EXPECT_EQ(av_sync_->source_offset(1), base::TimeDelta());
  EXPECT_EQ(av_sync_->SourceTime(1, base::Seconds(3)), base::Seconds(3));

  // Source 0 cannot be offset: it IS the master timeline.
  renderer_->SetSourceOffset(0, base::Seconds(5));
  EXPECT_EQ(av_sync_->source_offset(0), base::TimeDelta());

  // A stated offset maps source 1's pts onto the master timeline...
  renderer_->SetSourceOffset(1, base::Seconds(2));
  EXPECT_EQ(av_sync_->source_offset(1), base::Seconds(2));
  EXPECT_EQ(av_sync_->SourceTime(1, base::Seconds(3)), base::Seconds(5));

  // ...and reaches both legs' compositors as a per-leg clock. The tolerance
  // covers the two legs' readbacks landing on different push ticks (the
  // master extrapolates ~10 ms per tick); a mutation that drops the offset
  // from the push shows up as a difference near ZERO, and a sign flip as a
  // negative one, so 0.5 s around 2 s separates all three cases.
  renderer_->StartPlayingFrom(base::TimeDelta());
  VideoRendererImpl* leg0 = renderer_->video_renderer(0);
  VideoRendererImpl* leg1 = renderer_->video_renderer(1);
  ASSERT_NE(leg0, nullptr);
  ASSERT_NE(leg1, nullptr);
  ASSERT_TRUE(WaitFor([leg0, leg1] {
    return !leg0->master_clock().is_min() && !leg1->master_clock().is_min();
  })) << "a leg never received a master-clock push";
  const base::TimeDelta difference =
      leg0->master_clock() - leg1->master_clock();
  EXPECT_NEAR(difference.InSecondsF(), 2.0, 0.5)
      << "leg 1's clock is not the master minus its offset";
}

// The behavioural half, end to end: with a 2 s offset on source 1, that
// leg's SECOND frame is held for roughly the offset, because its clock reads
// (master - 2 s) while its frames' pts sit near zero. The first frame still
// presents immediately -- a null frame_timer has no reference to pace
// against (DecideNextFrame step 9) -- which is exactly why the assertion is
// about the frames AFTER the first.
TEST_F(RendererMultiSourceTest, ASecondaryLegHoldsItsFramesForTheOffset) {
  SetTwoSources();
  CohereGenerations(4242);
  ScriptBuffers(/*buffers=*/40);
  CreateRenderer();
  Initialize();
  ASSERT_TRUE(WaitFor([this] { return init_done_; }));
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  pump_secondary_ = false;
  renderer_->SetSourceOffset(1, base::Seconds(2));
  renderer_->StartPlayingFrom(base::TimeDelta());

  // Determinism gate, not politeness: wait until leg 1's compositor holds
  // the offset-adjusted clock AND has a frame decoded. Pulling before the
  // push lands would set the first frame's frame_timer from the UNMAPPED
  // clock and the hold below would measure the wrong thing.
  ASSERT_TRUE(WaitFor([this] {
    VideoRendererImpl* leg1 = renderer_->video_renderer(1);
    return leg1 && !leg1->master_clock().is_min() && leg1->frames_pending() > 0;
  })) << "leg 1 never received the offset-adjusted clock with a frame ready";
  test::FakeVideoSink* secondary = extra_factory_->sinks()[0];

  // Now the leg is pulled. The window is bounded by the MASTER's position
  // rather than by wall time: the offset's effect legitimately ends once the
  // master reaches it, and a fast-pulling machine must not be able to
  // outrun the assertion into a false failure.
  pump_secondary_ = true;
  const base::TimeDelta hold_limit = base::Seconds(2) - base::Milliseconds(100);
  const auto wall_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  bool leaked = false;
  while (std::chrono::steady_clock::now() < wall_deadline &&
         av_sync_->GetMasterClock() < hold_limit) {
    PumpSinks();
    if (secondary->frames().size() > 1) {
      leaked = true;
      break;
    }
  }
  EXPECT_FALSE(leaked)
      << "the secondary leg presented past its first frame while the offset "
         "should still be holding it";
  EXPECT_EQ(secondary->frames().size(), 1u)
      << "the first frame presents immediately; every later one waits for "
         "the offset";

  // Recovery: clearing the offset lets the same leg flow again, which is
  // what keeps "the offset was applied" from being confused with "the leg is
  // wedged". The held frame's timer was set when it was first due, so this
  // waits out the remainder of the offset -- bounded by kWaitTimeout.
  renderer_->SetSourceOffset(1, base::TimeDelta());
  EXPECT_TRUE(WaitFor([secondary] { return secondary->frames().size() > 1; }))
      << "the secondary leg never resumed after the offset was cleared";
}

// Stage 3's source-aware sink contract: the renderer must wire each secondary
// leg (source id >= 1) through VideoRendererSinkFactory::CreateForSource(id,
// ...) -- NOT Create() -- so a GL host can bind leg 1 to its own scene layer,
// leg 2 to layer 2's, and so on. "The secondary leg got a sink" alone cannot
// tell Create() from CreateForSource() (both hand back a sink), so the witness
// is a factory that overrides CreateForSource to record the requested id: if
// the renderer had called Create(), source_ids() would be empty and this test
// would go red. That is the deliberately-run mutation (see the closed
// regression note on docs/14 stage 3): the assertion is not vacuous.
//
// The assertion pairs the structural record (exactly one CreateForSource(1)
// call) with the behavioural half already proven by
// EverySourceGetsItsOwnVideoChain (the sink bound to source 1 carries source
// 1's frames): either alone is the wrong bug. A record of {1} with no frames
// is a factory that was asked but never fed; frames in a sink whose id was
// never recorded is a sink the renderer built through the wrong path.
TEST_F(RendererMultiSourceTest, SecondaryLegRoutesThroughSourceAwareSink) {
  SetTwoSources();
  ScriptBuffers(/*buffers=*/2);
  source_aware_factory_ = std::make_shared<test::SourceAwareVideoSinkFactory>();
  CreateRendererWith(MakeDepsWith(source_aware_factory_));
  Initialize();
  ASSERT_TRUE(WaitFor([this] { return init_done_; }));
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  // Structural half: the renderer took the source-aware path exactly once, for
  // the one secondary leg (source 1). Source 0 never reaches here -- it takes
  // the dedicated host sink first -- so {1} and nothing else is the signature
  // of the contract holding.
  const std::vector<int> ids = source_aware_factory_->source_ids();
  ASSERT_EQ(ids.size(), 1u)
      << "the renderer did not request exactly one secondary sink";
  EXPECT_EQ(ids[0], 1)
      << "the secondary leg was wired with the wrong source id";

  // Behavioural half: the sink that path created actually carries source 1's
  // frames (the serial is the sink-ownership witness from
  // EverySourceGetsItsOwnVideoChain). A factory that recorded {1} but was fed
  // source 0's frames would mean the id and the leg were decoupled.
  ASSERT_EQ(source_aware_factory_->sinks().size(), 1u);
  test::FakeVideoSink* secondary = source_aware_factory_->sinks()[0];
  renderer_->StartPlayingFrom(base::TimeDelta());
  ASSERT_TRUE(WaitFor([secondary] { return !secondary->frames().empty(); }))
      << "the source-aware sink never received a frame";
  for (const base::scoped_refptr<VideoFrame>& frame : secondary->frames()) {
    EXPECT_EQ(frame->serial(), kSecondaryVideoSerial)
        << "the source-aware sink received a frame that is not source 1's";
  }
}

}  // namespace
}  // namespace avbase::media
