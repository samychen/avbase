// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// AudioRendererImpl's half of the renderer contract, with no FFmpeg and one
// thread. The "device" is a fake sink the test pulls one period at a time,
// which is what makes the ring observable: every assertion here is about what
// reached the device, not about what the decoder produced.
//
// Four behaviours the tenth round could only reach through a real file:
//   * #41 -- a paused renderer reports silence and must not count it as an
//     underrun (the first draft would have filled the glitch counter at device
//     rate for as long as the user stayed paused);
//   * #42 -- the pump stops on ring back-pressure and only the *consumer* can
//     see the pressure clear, so consuming has to wake it;
//   * #2 in docs/PROGRESS §(5) -- a chunk from the flushed generation must not
//     reach the device after a seek;
//   * #48 -- the frames an EOS leaves in the algorithm have to be published, or
//     the pipeline waits for an end that never comes.
//
// Single-threaded: ~AudioRendererImpl only stops its sink, so no second thread
// is needed. Its audio clock is real (AvSyncController), because Render()
// anchors it and half these assertions are about what that implies.

#include "media/filters/audio_renderer_impl.h"

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
#include "media/base/audio_bus.h"
#include "media/base/audio_decoder_factory.h"
#include "media/base/audio_parameters.h"
#include "media/base/decoder_buffer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/pipeline_status.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "tests/support/fake_decoder_factories.h"
#include "tests/support/fake_demuxer_stream.h"
#include "tests/support/fake_renderer_sinks.h"

namespace avbase::media {
namespace {

// The device period. Deliberately smaller than the 1024 frames the fake decoder
// emits per buffer: that ratio is what puts a whole decoded buffer into the
// ring at once, and it is the configuration #48 needed (a period equal to the
// decoder's granularity hides the defect, which is why playback never hit it).
constexpr int kFramesPerPeriod = 256;
constexpr int kDecoderFramesPerBuffer = 1024;

class AudioRendererImplTest : public ::testing::Test {
 protected:
  void SetUp() override {
    runner_ = env_.GetMainThreadTaskRunnerRef();
    stream_ = std::make_unique<test::FakeDemuxerStream>(
        DemuxerStreamType::kAudio, test::MakeValidAudioConfig(),
        test::MakeValidVideoConfig());
    bus_ = AudioBus::Create(2, kFramesPerPeriod);
  }

  void ScriptBuffers(int buffers) {
    for (int i = 0; i < buffers; ++i) {
      stream_->AppendBuffer(test::MakeDataBuffer(DemuxerStreamType::kAudio,
                                                 stream_->serial()));
    }
  }

  void CreateRenderer() {
    std::vector<base::scoped_refptr<AudioDecoderFactory>> factories;
    factory_ = base::MakeRefCounted<test::FakeAudioDecoderFactory>(behaviour_);
    factories.push_back(factory_);
    av_sync_ = std::make_shared<AvSyncController>(
        AvSyncController::MasterType::kAudio, env_.GetTickClock(),
        AvSyncController::Thresholds());
    renderer_ = std::make_unique<AudioRendererImpl>(
        runner_, std::move(factories), av_sync_.get());
    renderer_->set_ended_cb(base::BindRepeating(
        &AudioRendererImplTest::OnEnded, base::Unretained(this)));
    sink_ = base::MakeRefCounted<test::FakeAudioSink>();
    renderer_->Initialize(
        stream_.get(),
        AudioParameters(ChannelLayout::kStereo, SampleFormat::kF32P, 48000,
                        kFramesPerPeriod),
        sink_,
        base::BindOnce(&AudioRendererImplTest::OnInitialized,
                       base::Unretained(this)));
    DrainQueue();
  }

  void OnInitialized(PipelineStatus status) {
    init_status_ = status;
    init_done_ = true;
  }

  void OnEnded() { ++ended_calls_; }

  // Runs the posted chain to quiescence: every hop in this class is a task, so
  // a single RunUntilIdle() usually leaves the next one still queued.
  void DrainQueue(int rounds = 4) {
    for (int i = 0; i < rounds; ++i) {
      env_.RunUntilIdle();
    }
  }

  void StartPlaying() {
    renderer_->StartPlayingFrom(base::TimeDelta());
    DrainQueue();
  }

  // One device period, as the sink would pull it.
  int PullPeriod() {
    const int frames = sink_->PullPeriod(bus_.get());
    if (frames > 0) {
      frames_consumed_ += frames;
    }
    return frames;
  }

  // Pulls until |predicate| holds or |max_periods| are consumed.
  template <typename Predicate>
  bool PullUntil(Predicate predicate, int max_periods = 200) {
    for (int i = 0; i < max_periods; ++i) {
      if (predicate()) {
        return true;
      }
      PullPeriod();
      DrainQueue();
    }
    return predicate();
  }

  base::test::TaskEnvironment env_;
  base::scoped_refptr<base::SequencedTaskRunner> runner_;
  test::FakeDecoderBehaviour behaviour_;
  base::scoped_refptr<test::FakeAudioDecoderFactory> factory_;
  std::shared_ptr<AvSyncController> av_sync_;
  std::unique_ptr<test::FakeDemuxerStream> stream_;
  std::unique_ptr<AudioRendererImpl> renderer_;
  base::scoped_refptr<test::FakeAudioSink> sink_;
  std::unique_ptr<AudioBus> bus_;
  bool init_done_ = false;
  PipelineStatus init_status_ = PipelineStatus::kOk;
  int ended_calls_ = 0;
  int frames_consumed_ = 0;
};

// Baselines: audio decodes, the device gets it, the drained stream reports
// ended once, and the algorithm is empty afterwards -- all four numbers are
// what the four behaviours below perturb.
TEST_F(AudioRendererImplTest, DecodesConsumesAndEnds) {
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer();
  ASSERT_TRUE(init_done_);
  ASSERT_EQ(init_status_, PipelineStatus::kOk);
  // The device is not opened by Initialize: a decoder that fails to come up
  // must not leave a device pulling silence (audio_renderer_impl.cc:148).
  ASSERT_EQ(sink_->callback(), nullptr);

  StartPlaying();
  EXPECT_NE(sink_->callback(), nullptr);
  EXPECT_EQ(sink_->start_count(), 1);

  EXPECT_TRUE(PullUntil([this] { return renderer_->ended(); }));
  EXPECT_EQ(ended_calls_, 1);
  // ended() is reported when the stream drains, which is *before* the last
  // buffer reaches the device -- so the totals below wait for the ring and the
  // algorithm to empty as well.
  EXPECT_TRUE(PullUntil([this] { return renderer_->buffered_frames() == 0; }));
  // Every frame the decoder emitted reached the device: 2 buffers per input
  // buffer, 1024 frames each.
  EXPECT_EQ(frames_consumed_, 2 * kDecoderFramesPerBuffer);
}

// #41: paused means "play silence", and silence is not an underrun. Counting it
// would fill the glitch statistics at device rate for as long as the user stays
// paused, which is exactly the kind of number that makes a real underrun
// invisible.
TEST_F(AudioRendererImplTest, PausePlaysSilenceWithoutCountingAnUnderrun) {
  ScriptBuffers(/*buffers=*/1);
  CreateRenderer();
  ASSERT_EQ(init_status_, PipelineStatus::kOk);
  StartPlaying();

  ASSERT_TRUE(PullUntil([this] { return frames_consumed_ > 0; }));
  const uint64_t underruns_before = renderer_->underruns();

  renderer_->SetPaused(true);
  DrainQueue();
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(PullPeriod(), 0) << "a paused renderer wrote to the device";
    DrainQueue();
  }
  EXPECT_EQ(renderer_->underruns(), underruns_before)
      << "pause was counted as an underrun";

  renderer_->SetPaused(false);
  DrainQueue();
  EXPECT_TRUE(PullUntil([this] { return renderer_->ended(); }));
}

// #42: the pump stops when the ring is full, and only the consumer can observe
// that the pressure cleared -- so consuming a period has to be what wakes the
// producer. Without the wake-up (the counterpart of ffplay's
// frame_queue_signal) a full ring is a permanent stall.
TEST_F(AudioRendererImplTest, ConsumptionWakesTheStalledPump) {
  ScriptBuffers(/*buffers=*/2);
  CreateRenderer();
  ASSERT_EQ(init_status_, PipelineStatus::kOk);

  StartPlaying();
  // Saturate the ring first: one decoded buffer is four periods long, so a
  // single period of consumption is not enough to tell "the pump resumed" from
  // "the ring had it already".
  ASSERT_GT(PullPeriod(), 0);
  DrainQueue();

  // Count the periods the device can still get. The ring alone holds
  // AudioRendererImpl::kReadyChunks (4) of them; everything beyond that had to
  // be published *after* the pump stopped, which only the consumer's wake-up
  // can trigger. Reading stream_->read_count() instead would prove nothing:
  // DecoderStream satisfies a Read from its own decoded outputs or from its
  // preload buffer without touching the demuxer stream.
  // Pull until the renderer has nothing left anywhere. The loop is bounded so a
  // stall fails the test instead of hanging it, and it deliberately does not
  // stop at ended(): the stream reaches EOS on the read *after* the ring
  // filled, so most of the audio is still buffered when ended() turns true.
  const int total_buffers = 2 * behaviour_.outputs_per_buffer;
  int periods_with_data = 0;
  for (int i = 0; i < 200 && renderer_->buffered_frames() > 0; ++i) {
    if (PullPeriod() > 0) {
      ++periods_with_data;
    }
    DrainQueue();
  }
  EXPECT_EQ(renderer_->buffered_frames(), 0)
      << "frames stayed buffered with nothing left to publish them";
  EXPECT_GT(periods_with_data, 4)
      << "the device starved as soon as the ring drained: consuming a period "
         "did not wake the stalled pump";
  EXPECT_EQ(frames_consumed_, total_buffers * kDecoderFramesPerBuffer);
}

// The seek half of #2: Flush() clears the ring, pauses and flushes the sink,
// and the next generation is stamped with the new serial. A chunk that survived
// the flush would be played at the new position -- audible as a burst of the
// old audio, the audio twin of the NAL corruption after a seek.
TEST_F(AudioRendererImplTest, FlushDropsTheRingAndPausesTheSink) {
  ScriptBuffers(/*buffers=*/2);
  CreateRenderer();
  ASSERT_EQ(init_status_, PipelineStatus::kOk);
  StartPlaying();
  ASSERT_TRUE(PullUntil([this] { return frames_consumed_ > 0; }));
  const int pause_count_before = sink_->pause_count();

  stream_->set_serial(1);
  bool flush_done = false;
  renderer_->Flush(base::BindOnce([](bool* done) { *done = true; },
                                  base::Unretained(&flush_done)));
  DrainQueue();

  EXPECT_TRUE(flush_done);
  EXPECT_EQ(renderer_->buffered_frames(), 0) << "the ring survived the flush";
  EXPECT_GT(sink_->pause_count(), pause_count_before);
  EXPECT_GT(sink_->flush_count(), 0);
  EXPECT_FALSE(renderer_->ended());
}

// #48, directly: the ring can be full at the instant EOS arrives, and the
// frames the algorithm still holds then have nobody to publish them -- the pump
// has stopped (ended_) and PreStretch() is only called on a decode or on
// resume. buffered_frames() therefore never returned to zero and the pipeline
// waited forever. This is the same defect the RendererImpl suite found through
// OnEnded, asserted here on the count it actually blocks on.
TEST_F(AudioRendererImplTest, EndedPublishesTheTailTheRingCouldNotTake) {
  ScriptBuffers(/*buffers=*/1);
  // Two outputs per input buffer, so the tail is a whole buffer.
  behaviour_.outputs_per_buffer = 2;
  CreateRenderer();
  ASSERT_EQ(init_status_, PipelineStatus::kOk);
  StartPlaying();

  // Fill the ring first, then let EOS arrive against a full ring: that is the
  // condition the defect needs, and the first period below is what creates it.
  ASSERT_GT(PullPeriod(), 0);
  DrainQueue();

  EXPECT_TRUE(PullUntil([this] { return renderer_->ended(); }))
      << "the stream never reached ended";
  EXPECT_EQ(renderer_->ended(), true);
  // The tail has to come out *after* EOS: if the fix is reverted this loop
  // drains the ring and stops, leaving buffered_frames() at one decoded buffer
  // forever.
  EXPECT_TRUE(PullUntil([this] { return renderer_->buffered_frames() == 0; }))
      << "frames left in the algorithm after EOS were never published";
  EXPECT_EQ(frames_consumed_, 2 * kDecoderFramesPerBuffer);
}

}  // namespace
}  // namespace avbase::media
