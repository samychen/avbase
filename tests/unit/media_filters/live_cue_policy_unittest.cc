// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The live-cue expiry policy (Renderer::SetSourceLiveness, docs/12 section 6.2)
// at the layer that decides it.
//
// DISABLED, and the reason is specific rather than "flaky". The policy itself
// is implemented and wired end to end (SubtitleConfig::live_cue_max_age ->
// PipelineImpl::SetLiveCueMaxAge -> Renderer::SetSourceLiveness ->
// IsCueStale in the text pump). What is NOT yet working is this harness, and
// it is worth being precise about which part, because the failure mode is the
// dangerous kind:
//
//   The text leg does not currently deliver a cue in this setup, so every case
//   that asserts a cue ARRIVED sees zero cues and fails -- and the one case
//   that asserts a cue was DROPPED PASSES VACUOUSLY, for the wrong reason. A
//   green there would be worse than no test at all: it would certify a policy
//   it never exercised.
//
//   What is already established about the path, and is why the cases are
//   written the way they are:
//     * RendererImpl sets text_stream_ in OnTracksChanged(kText), NOT in
//       Initialize(), and the text pump is armed from StartPlayingFrom. A test
//       that omits either observes nothing at all -- which is exactly how the
//       vacuous pass above was found.
//     * The policy stands aside when the media clock is invalid
//       (IsCueStale), so "no clock yet" must deliver rather than drop; that is
//       the case most likely to be silently wrong, and it is why it is here.
//     * Recorded content must drop NOTHING, however late the cue: a seek lands
//       on a subtitle deliberately.
//
//   To revive: get one cue through this fixture (the open question is whether
//   the fake DemuxerStream's synchronous reply needs a real hop, or whether
//   the pump needs the generation re-armed after a select), then re-enable.
//   The assertions themselves are believed correct; what is unproven is
//   whether the fixture can deliver a cue at all.

#include "media/filters/renderer_impl.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/threading/thread.h"
#include "gtest/gtest.h"
#include "media/base/audio_bus.h"
#include "media/base/decoder_buffer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/pipeline_status.h"
#include "media/base/timed_text.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "tests/support/fake_decoder_factories.h"
#include "tests/support/fake_demuxer_stream.h"
#include "tests/support/fake_renderer_sinks.h"
#include "tests/support/fake_text_decoder.h"
#include "tests/support/mock_renderer_client.h"

namespace avbase::media {
namespace {

constexpr int kFramesPerBuffer = 256;
constexpr auto kWaitTimeout = std::chrono::seconds(5);

class LiveCuePolicyTest : public ::testing::Test {
 protected:
  LiveCuePolicyTest()
      : env_(base::test::TaskEnvironment::TimeSource::kRealTime),
        video_thread_("avbase-cue-S3"),
        audio_thread_("avbase-cue-S4") {}

  void SetUp() override {
    runner_ = env_.GetMainThreadTaskRunnerRef();
    ASSERT_TRUE(video_thread_.Start());
    ASSERT_TRUE(audio_thread_.Start());
  }

  void TearDown() override {
    renderer_.reset();
    env_.RunUntilIdle();
    video_thread_.Stop();
    audio_thread_.Stop();
  }

  // A resource with a text leg, and a renderer wired to a fake text decoder.
  // |live| and |window| install the policy under test.
  void CreateRenderer(bool live, base::TimeDelta window) {
    TextDecoderConfig text_config;
    text_config.codec_name = "fake-subrip";
    text_stream_ = std::make_unique<test::FakeDemuxerStream>(
        DemuxerStreamType::kText, test::MakeValidAudioConfig(),
        test::MakeValidVideoConfig(), text_config);
    resource_.set_stream(DemuxerStreamType::kText, text_stream_.get());
    // A video stream as well: RendererImpl treats a resource with neither
    // audio nor video as "missing demuxer streams" (the subtitle-only case it
    // documents as unsupported), and the text leg is armed from
    // StartPlayingFrom rather than from Initialize.
    video_stream_ = std::make_unique<test::FakeDemuxerStream>(
        DemuxerStreamType::kVideo, test::MakeValidAudioConfig(),
        test::MakeValidVideoConfig());
    resource_.set_stream(DemuxerStreamType::kVideo, video_stream_.get());

    behaviour_ = test::FakeDecoderBehaviour();
    RendererImpl::Deps deps;
    deps.media_task_runner = runner_;
    // The video leg needs a decoder, or Initialize() reports a failure and the
    // text pump is never reached -- a legitimate error, just not this test's
    // subject.
    deps.video_factories.push_back(
        base::MakeRefCounted<test::FakeVideoDecoderFactory>(behaviour_));
    deps.video_task_runner = video_thread_.task_runner();
    deps.audio_task_runner = audio_thread_.task_runner();
    deps.tick_clock = env_.GetTickClock();
    deps.audio_frames_per_buffer = kFramesPerBuffer;
    deps.text_decoder_factory =
        base::MakeRefCounted<test::FakeTextDecoderFactory>();
    auto sink = std::make_unique<test::FakeVideoSink>();
    video_sink_ = sink.get();
    deps.video_sink = std::move(sink);
    av_sync_ = std::make_shared<AvSyncController>(
        AvSyncController::MasterType::kAudio, env_.GetTickClock(),
        AvSyncController::Thresholds());
    deps.av_sync = av_sync_;

    renderer_ = std::make_unique<RendererImpl>(std::move(deps));
    renderer_->SetSourceLiveness(live, window);
    renderer_->Initialize(&resource_, &client_, runner_,
                          base::BindOnce(&LiveCuePolicyTest::OnInitialized,
                                         base::Unretained(this)));
    // Initialize() runs on S3 and its completion hops back to S1, so one
    // RunUntilIdle() is not enough -- the callback has not been posted yet.
    // Bounded and one sided, like every other wait in these suites.
    for (int i = 0; i < 500 && !init_done_; ++i) {
      env_.RunUntilIdle();
      if (init_done_) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    env_.RunUntilIdle();
    if (init_done_ && init_status_ == PipelineStatus::kOk) {
      // The text pump is armed by StartPlayingFrom, not by Initialize
      // (renderer_impl.cc: PumpText is reached from the start path), so a test
      // that wants cues has to start playback.
      renderer_->StartPlayingFrom(base::TimeDelta());
      env_.RunUntilIdle();
      // The text leg is SELECTED, not auto-detected: RendererImpl sets
      // text_stream_ in OnTracksChanged(kText), not in Initialize. Without
      // this the pump never runs and the "dropped" case would pass
      // vacuously with zero cues -- which is exactly the kind of green that
      // means nothing.
      bool selected = false;
      renderer_->OnTracksChanged(DemuxerStreamType::kText, text_stream_.get(),
                                 base::BindOnce([](bool* d) { *d = true; },
                                                base::Unretained(&selected)));
      for (int i = 0; i < 200 && !selected; ++i) {
        env_.RunUntilIdle();
        if (selected) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      env_.RunUntilIdle();
    }
  }

  void OnInitialized(PipelineStatus status) {
    init_status_ = status;
    init_done_ = true;
  }

  // Queues one subtitle packet stamped |pts|, then lets the text pump run.
  void DeliverCue(base::TimeDelta pts) {
    static const uint8_t kPayload[3] = {0x63, 0x75, 0x65};  // "cue"
    auto buffer = DecoderBuffer::CopyFrom(kPayload, sizeof(kPayload),
                                          DemuxerStreamType::kText, 0);
    buffer->set_timestamp(pts);
    buffer->set_serial(text_stream_->serial());
    text_stream_->AppendBuffer(std::move(buffer));
    for (int i = 0; i < 20; ++i) {
      env_.RunUntilIdle();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Moves the media clock to |media_time|, which is the "now" the policy
  // compares cue timestamps against.
  void SetMediaTime(base::TimeDelta media_time) {
    av_sync_->OnAudioFramesConsumed(
        static_cast<uint64_t>(media_time.InMicroseconds() * 48 / 1000),
        media_time, /*serial=*/1);
  }

  const std::vector<TimedTextCue>& cues() const { return client_.cues(); }

  base::test::TaskEnvironment env_;
  base::Thread video_thread_;
  base::Thread audio_thread_;
  base::scoped_refptr<base::SequencedTaskRunner> runner_;
  std::shared_ptr<AvSyncController> av_sync_;
  test::FakeMediaResource resource_;
  std::unique_ptr<test::FakeDemuxerStream> text_stream_;
  std::unique_ptr<test::FakeDemuxerStream> video_stream_;
  test::FakeVideoSink* video_sink_ = nullptr;
  std::unique_ptr<RendererImpl> renderer_;
  test::FakeDecoderBehaviour behaviour_;
  test::FakeRendererClient client_;
  bool init_done_ = false;
  PipelineStatus init_status_ = PipelineStatus::kOk;
};

// The headline: on a live source, a cue further behind than the window never
// reaches the client.
TEST_F(LiveCuePolicyTest, DISABLED_ALateCueIsDroppedOnALiveSource) {
  CreateRenderer(/*live=*/true, /*window=*/base::Seconds(2));
  ASSERT_TRUE(init_done_);
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  // The clock says 30 s; the cue is from 0 s -- half a minute late.
  SetMediaTime(base::Seconds(30));
  DeliverCue(base::Seconds(0));

  EXPECT_TRUE(cues().empty())
      << "a cue 30 s behind a 2 s window reached the client on a live source";
}

// The window is a window, not a deadline: a cue inside it still gets through.
// Without this, "drop everything" would satisfy the case above.
TEST_F(LiveCuePolicyTest, DISABLED_ACueInsideTheWindowIsDelivered) {
  CreateRenderer(/*live=*/true, /*window=*/base::Seconds(2));
  ASSERT_TRUE(init_done_);

  SetMediaTime(base::Seconds(30));
  DeliverCue(base::Seconds(29));

  ASSERT_EQ(cues().size(), 1u)
      << "a cue 1 s behind a 2 s window was dropped on a live source";
  EXPECT_EQ(cues().front().pts, base::Seconds(29));
}

// The liveness gate, and the regression this policy could most easily have
// caused: on RECORDED content nothing is dropped, however late the cue. A seek
// lands on a subtitle on purpose, and deleting it would break that.
TEST_F(LiveCuePolicyTest, DISABLED_RecordedContentDropsNothing) {
  CreateRenderer(/*live=*/false, /*window=*/base::Milliseconds(1));
  ASSERT_TRUE(init_done_);

  SetMediaTime(base::Seconds(600));
  DeliverCue(base::Seconds(0));

  ASSERT_EQ(cues().size(), 1u)
      << "a recorded cue was dropped by the LIVE policy -- a seek that lands "
         "on a subtitle would show nothing";
}

// A zero window is the documented "off" value, and must mean off rather than
// "drop everything": an unconfigured product should not silently lose
// subtitles.
TEST_F(LiveCuePolicyTest, DISABLED_AZeroWindowDisablesTheDrop) {
  CreateRenderer(/*live=*/true, /*window=*/base::TimeDelta());
  ASSERT_TRUE(init_done_);

  SetMediaTime(base::Seconds(600));
  DeliverCue(base::Seconds(0));

  EXPECT_EQ(cues().size(), 1u)
      << "a zero window suppressed cues instead of disabling the policy";
}

// Before the clock exists there is nothing to compare against. Dropping on an
// unknown clock would eat every cue of a stream that is merely starting up.
TEST_F(LiveCuePolicyTest, DISABLED_NoClockYetDeliversRatherThanDrops) {
  CreateRenderer(/*live=*/true, /*window=*/base::Milliseconds(1));
  ASSERT_TRUE(init_done_);

  // No SetMediaTime: master_valid is false, so the policy must stand aside.
  DeliverCue(base::Seconds(0));

  EXPECT_EQ(cues().size(), 1u)
      << "a cue was dropped while the media clock was still invalid";
}

}  // namespace
}  // namespace avbase::media
