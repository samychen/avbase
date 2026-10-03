// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// RendererImpl's orchestration contract, with no FFmpeg. The tenth round's
// renderer defects were all found by playing a real file: #36 (natural EOS
// treated as "nothing to do", so a finished file never reached OnEnded), #37
// (the init callback died in a returning stack frame when video finished before
// audio), #38/#39 (callbacks and destruction on the wrong sequence). Playing a
// file is a slow, indirect oracle for all four; this suite drives the same
// component directly.
//
// WHY REAL THREADS, which looks like more machinery than a fake runner:
//   * ~RendererImpl posts each sub-renderer's destruction to its own sequence
//     and waits for it to run (DestroyOn, renderer_impl.cc). A single-threaded
//     runner deadlocks there, because the thread that must run the task is the
//     one blocked in the wait.
//   * S3/S4 are real threads in production, so this is the production shape.
//   * It is also the only shape in which the DECODER_STREAM SEQUENCE_CHECKERs
//     can disagree, which is what #38/#39 were about.
// The cost is real time: the renderer's 10 ms clock push is a real delay here,
// so waits are bounded rather than exact. Every wait below is bounded and one
// sided (it waits for a state the component must reach), which is what keeps
// the suite deterministic instead of racy.

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
#include "tests/support/fake_decoder_factories.h"
#include "tests/support/fake_demuxer_stream.h"
#include "tests/support/fake_renderer_sinks.h"
#include "tests/support/mock_renderer_client.h"

namespace avbase::media {
namespace {

// Chunk size for the audio ring. Smaller than the 1024-frame default so that
// draining it in a test takes a handful of periods rather than a page of them.
constexpr int kFramesPerBuffer = 256;

// Ceiling for every wait. Generous, because it is a ceiling and not a delay:
// the tests pass as soon as the state is reached.
constexpr auto kWaitTimeout = std::chrono::seconds(5);

class RendererImplTest : public ::testing::Test {
 protected:
  RendererImplTest()
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

  // Configures the resource the renderer reads from. false models the
  // audio-only / video-only cases, which are nullptr and not an error
  // (media/base/media_resource.h).
  void SetStreams(bool with_video, bool with_audio) {
    if (with_video) {
      video_stream_ = std::make_unique<test::FakeDemuxerStream>(
          DemuxerStreamType::kVideo, test::MakeValidAudioConfig(),
          test::MakeValidVideoConfig());
      resource_.set_stream(DemuxerStreamType::kVideo, video_stream_.get());
    }
    if (with_audio) {
      audio_stream_ = std::make_unique<test::FakeDemuxerStream>(
          DemuxerStreamType::kAudio, test::MakeValidAudioConfig(),
          test::MakeValidVideoConfig());
      resource_.set_stream(DemuxerStreamType::kAudio, audio_stream_.get());
    }
  }

  // |buffers| worth of scripted data per configured stream, then EOS.
  void ScriptBuffers(int buffers, int outputs_per_buffer = 2) {
    behaviour_.outputs_per_buffer = outputs_per_buffer;
    for (int i = 0; i < buffers; ++i) {
      if (video_stream_) {
        video_stream_->AppendBuffer(test::MakeDataBuffer(
            DemuxerStreamType::kVideo, video_stream_->serial()));
      }
      if (audio_stream_) {
        audio_stream_->AppendBuffer(test::MakeDataBuffer(
            DemuxerStreamType::kAudio, audio_stream_->serial()));
      }
    }
  }

  RendererImpl::Deps MakeDeps(bool with_video, bool with_audio) {
    RendererImpl::Deps deps;
    deps.media_task_runner = runner_;
    deps.tick_clock = env_.GetTickClock();
    deps.audio_frames_per_buffer = kFramesPerBuffer;
    deps.video_disabled = !with_video;
    deps.audio_disabled = !with_audio;
    if (with_video) {
      deps.video_task_runner = video_thread_.task_runner();
      video_factory_ =
          base::MakeRefCounted<test::FakeVideoDecoderFactory>(behaviour_);
      deps.video_factories.push_back(video_factory_);
      auto sink = std::make_unique<test::FakeVideoSink>();
      video_sink_ = sink.get();
      deps.video_sink = std::move(sink);
    } else {
      deps.video_task_runner = video_thread_.task_runner();
    }
    if (with_audio) {
      deps.audio_task_runner = audio_thread_.task_runner();
      audio_factory_ =
          base::MakeRefCounted<test::FakeAudioDecoderFactory>(behaviour_);
      deps.audio_factories.push_back(audio_factory_);
      audio_sink_ = base::MakeRefCounted<test::FakeAudioSink>();
      deps.audio_sink =
          base::scoped_refptr<AudioRendererSink>(audio_sink_.get());
    } else {
      deps.audio_task_runner = audio_thread_.task_runner();
    }
    av_sync_ = std::make_shared<AvSyncController>(
        AvSyncController::MasterType::kAudio, env_.GetTickClock(),
        AvSyncController::Thresholds());
    deps.av_sync = av_sync_;
    return deps;
  }

  void CreateRenderer(bool with_video, bool with_audio) {
    renderer_ =
        std::make_unique<RendererImpl>(MakeDeps(with_video, with_audio));
  }

  // Initialize() with this suite's callback. Nothing is drained here: "what ran
  // before Initialize returned" is what two of these tests are about.
  void Initialize() {
    renderer_->Initialize(&resource_, &client_, runner_,
                          base::BindOnce(&RendererImplTest::OnInitialized,
                                         base::Unretained(this)));
  }

  void OnInitialized(PipelineStatus status) {
    ++init_calls_;
    init_status_ = status;
    init_done_ = true;
  }

  // Pumps the media sequence while waiting for |predicate|, optionally pulling
  // the sinks as a device and a display would. Bounded: a stall fails the test
  // instead of hanging it (the discipline decoder_stream_unittest.cc set).
  template <typename Predicate>
  bool WaitFor(Predicate predicate, bool pump_sinks = true,
               std::chrono::milliseconds timeout = kWaitTimeout) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
      env_.RunUntilIdle();
      if (predicate()) {
        return true;
      }
      if (pump_sinks) {
        PumpSinks();
      }
      if (std::chrono::steady_clock::now() - start > timeout) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // One display interval's worth of frames plus one audio device period, as the
  // two endpoints would ask for them.
  void PumpSinks() {
    if (video_sink_) {
      video_sink_->PullFrames(4);
    }
    if (audio_sink_) {
      PullAudioPeriod();
    }
  }

  int PullAudioPeriod() {
    if (!audio_bus_) {
      audio_bus_ = AudioBus::Create(2, kFramesPerBuffer);
    }
    if (!audio_sink_) {
      return 0;
    }
    const int frames = audio_sink_->PullPeriod(audio_bus_.get());
    if (frames > 0) {
      ++audio_periods_with_data_;
    }
    return frames;
  }

  // The two repeating chains (PushMasterClock at 10 ms, PushStatistics at 1 s,
  // renderer_impl.cc) re-post themselves until ended_ is set, and they only
  // stop at end of playback -- NOT at teardown. A test that starts playback
  // without draining to EOS therefore never sees a quiet S1 queue, and an
  // earlier version of this function waited for exactly that: it sat out its
  // whole 15 s ceiling and returned false, which cost the suite 15 s per
  // such test (measured: StartPlayingFromOpensTheAudioDevice took 475 ms alone
  // and 15004 ms in suite order -- deterministic, because suite order decides
  // whether the preceding test left the sinks drained).
  //
  // That wait was only ever a proxy for "no queued callback names this
  // renderer", and the proxy became unnecessary when the S1 hops were weak
  // bound (see the note above RendererImpl::PostVideoEnded): a chain tick
  // still queued after ~RendererImpl is now inert instead of fatal. So drain
  // what is ready -- which is what actually observes state the test cares
  // about -- and destroy. The short ceiling stays as a backstop that fails
  // loudly instead of hanging; it is never expected to elapse.
  void DestroyRenderer() {
    if (!renderer_) {
      return;
    }
    // Bounded and one sided: run what is ready, then go. A test that reached
    // EOS has already had its chains retire, so this returns immediately; one
    // that did not still has a tick in flight, and that is now harmless.
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

  // Declaration order matters: the threads and the environment must outlive the
  // renderer they are handed to, and ~RendererImpl blocks on the two threads.
  base::test::TaskEnvironment env_;
  base::Thread video_thread_;
  base::Thread audio_thread_;
  base::scoped_refptr<base::SequencedTaskRunner> runner_;
  test::FakeDecoderBehaviour behaviour_;
  base::scoped_refptr<test::FakeVideoDecoderFactory> video_factory_;
  base::scoped_refptr<test::FakeAudioDecoderFactory> audio_factory_;
  std::shared_ptr<AvSyncController> av_sync_;
  test::FakeVideoSink* video_sink_ = nullptr;
  base::scoped_refptr<test::FakeAudioSink> audio_sink_;
  test::FakeMediaResource resource_;
  std::unique_ptr<test::FakeDemuxerStream> video_stream_;
  std::unique_ptr<test::FakeDemuxerStream> audio_stream_;
  std::unique_ptr<RendererImpl> renderer_;
  test::FakeRendererClient client_;
  std::unique_ptr<AudioBus> audio_bus_;
  int audio_periods_with_data_ = 0;
  int init_calls_ = 0;
  bool init_done_ = false;
  PipelineStatus init_status_ = PipelineStatus::kOk;
};

// A subtitle-only input, or a source whose only stream is text: there is no
// TextRenderer (gap 3 in media/base/media_resource.h), so the renderer cannot
// play it. It must say so through the documented status.
TEST_F(RendererImplTest, MissingStreamsAreReportedNotIgnored) {
  CreateRenderer(/*with_video=*/false, /*with_audio=*/false);
  Initialize();

  EXPECT_TRUE(WaitFor([this] { return init_done_; }));
  EXPECT_EQ(init_calls_, 1);
  EXPECT_EQ(init_status_, PipelineStatus::kMissingDemuxerStreams);
}

// renderer.h:111 -- "|init_cb| runs on the media sequence and is never run
// inline, so a caller may safely destroy state in it". The missing-streams path
// is still a path: a caller that destroys its own state in the callback would
// otherwise be re-entered while Initialize() is still on the stack. This test
// failed against the inline version.
TEST_F(RendererImplTest, InitCallbackIsNeverRunInline) {
  CreateRenderer(/*with_video=*/false, /*with_audio=*/false);
  Initialize();

  EXPECT_FALSE(init_done_) << "init_cb ran inside Initialize()";
  EXPECT_TRUE(WaitFor([this] { return init_done_; }));
}

// #37 and #38: with both streams the renderer waits for both sub-renderers and
// reports startup exactly once, from a hop -- not from whichever sub-renderer
// happened to finish last, and not from the one that finished first either.
TEST_F(RendererImplTest, InitIsReportedOnceWithBothStreams) {
  SetStreams(/*with_video=*/true, /*with_audio=*/true);
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer(/*with_video=*/true, /*with_audio=*/true);
  Initialize();

  EXPECT_FALSE(init_done_);
  EXPECT_TRUE(WaitFor([this] { return init_done_; }));
  EXPECT_EQ(init_calls_, 1);
  EXPECT_EQ(init_status_, PipelineStatus::kOk);
  // Both sub-renderers were built, and both decoder factories were asked for a
  // decoder.
  EXPECT_EQ(video_factory_->counters().init_calls, 1);
  EXPECT_EQ(audio_factory_->counters().init_calls, 1);
  ASSERT_TRUE(video_sink_);
  EXPECT_NE(video_sink_->callback(), nullptr);
  // But the audio *device* is not open yet, and that asymmetry is deliberate:
  // AudioRendererImpl initializes and starts its sink in StartPlayingFrom, so
  // that a decoder that fails to initialize cannot leave a device open and
  // pulling silence (audio_renderer_impl.cc:148).
  ASSERT_TRUE(audio_sink_);
  EXPECT_EQ(audio_sink_->callback(), nullptr);
  EXPECT_EQ(audio_sink_->start_count(), 0);
}

// ...and the other half of that asymmetry: playback start is what opens it.
TEST_F(RendererImplTest, StartPlayingFromOpensTheAudioDevice) {
  SetStreams(/*with_video=*/false, /*with_audio=*/true);
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer(/*with_video=*/false, /*with_audio=*/true);
  Initialize();
  ASSERT_TRUE(WaitFor([this] { return init_done_; }));
  ASSERT_EQ(init_status_, PipelineStatus::kOk);
  ASSERT_TRUE(audio_sink_);
  ASSERT_EQ(audio_sink_->callback(), nullptr);

  renderer_->StartPlayingFrom(base::TimeDelta());

  EXPECT_TRUE(WaitFor([this] { return audio_sink_->start_count() == 1; }));
  EXPECT_NE(audio_sink_->callback(), nullptr);
}

// An audio-only resource must initialize, and must not build the video half.
TEST_F(RendererImplTest, AudioOnlyResourceInitializes) {
  SetStreams(/*with_video=*/false, /*with_audio=*/true);
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer(/*with_video=*/false, /*with_audio=*/true);
  Initialize();

  EXPECT_TRUE(WaitFor([this] { return init_done_; }));
  EXPECT_EQ(init_status_, PipelineStatus::kOk);
  EXPECT_EQ(video_sink_, nullptr);
  EXPECT_EQ(video_factory_, nullptr);
  ASSERT_TRUE(audio_sink_);
}

// #36: a stream that decodes its last buffer and then drains must reach
// OnEnded(). Before that fix, video pumped forever and audio was never marked
// ended, so a finished file never completed.
TEST_F(RendererImplTest, EndedIsReportedAfterBothStreamsDrain) {
  SetStreams(/*with_video=*/true, /*with_audio=*/true);
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer(/*with_video=*/true, /*with_audio=*/true);
  Initialize();
  ASSERT_TRUE(WaitFor([this] { return init_done_; }));
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  renderer_->StartPlayingFrom(base::TimeDelta());

  // The wait itself pumps both sinks, which is what lets the ring and the
  // compositor drain; the renderer's own 10 ms clock push re-evaluates the
  // ended condition between pumps.
  EXPECT_TRUE(WaitFor([this] { return client_.Has("ended"); }))
      << "no OnEnded after both streams drained";
  // The intermediates, asserted because they are what "drained" means: both
  // demuxer streams were read past their single buffer into EOS, both decoded
  // frames were presented, and the audio device was opened and fully consumed.
  //
  // These counts are where the tail-frame defect showed up: the audio decoder
  // emits 1024-frame buffers while the device period is 256 frames, so eight
  // periods carry the decoded audio, and the last two could not leave the
  // algorithm until AudioRendererImpl::PumpDecoder() started publishing after
  // EOS. Before that fix this test reached "ended = 0" with eight periods
  // decoded but only six delivered.
  EXPECT_EQ(video_stream_->read_count(), 2);
  EXPECT_EQ(audio_stream_->read_count(), 2);
  EXPECT_EQ(video_sink_->frames().size(), 2u);
  EXPECT_EQ(audio_sink_->start_count(), 1);
  EXPECT_EQ(audio_periods_with_data_, 8);
  EXPECT_TRUE(renderer_->ended());
  EXPECT_EQ(client_.Count("ended"), 1);
  // The drain path must not have taken a failure branch on the way here: a
  // renderer reporting ended *and* an error would hide a real defect behind the
  // event the test is looking for.
  EXPECT_EQ(client_.Count("error"), 0);
}

}  // namespace
}  // namespace avbase::media
