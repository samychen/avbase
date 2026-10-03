// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/threading/thread.h"
#include "base/time/default_tick_clock.h"
#include "gtest/gtest.h"
#include "media/base/data_source.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/pipeline_status.h"
#include "media/filters/pipeline_impl.h"
#include "tests/support/fake_pipeline_client.h"
#include "tests/support/fake_renderer_sinks.h"
#include "tests/support/fake_sink_factories.h"
#include "tests/support/pipeline_fixture.h"

namespace avbase::media {
namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(45);
constexpr int kFramesPerBuffer = 256;
constexpr int kAudioChannels = 2;

// Phase 4: runtime audio-track switching. The fixture mirrors
// pipeline_throttle_unittest.cc; the media is a 3 s file with one video and
// TWO audio tracks (tests/testdata/two_audio_tracks.m4a), so a switch has a
// real alternate to hand over to and playback can continue to EOS afterwards.
class PipelineTrackTest : public PipelineTestFixture {
 protected:
  PipelineTrackTest() : PipelineTestFixture(/*ffmpeg_mode=*/true) {
    media_file_ = "two_audio_tracks.m4a";
  }

  // The container stream indices of the two audio tracks in the test media
  // (stream 0 and 1: audio-only).
  static constexpr int kTrackA = 0;
  static constexpr int kTrackB = 1;

  std::unique_ptr<AudioBus> bus_ =
      AudioBus::Create(kAudioChannels, kFramesPerBuffer);
};

TEST_F(PipelineTrackTest, DemuxerEnumeratesBothAudioTracks) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }))
      << "never probed the source; events:\n"
      << client_.EventLog();
  // Serialize test-thread pulls onto the renderer's own sequence; without
  // this the direct PullPeriod path races the renderer teardown (the same
  // arm-the-sink step the seek and throttle fixtures do).
  if (audio_sinks_->last_sink()) {
    audio_sinks_->last_sink()->set_render_runner(audio_thread_.task_runner());
  }

  // Player::SelectTrack validates the requested index against
  // MediaInfo::streams, so both tracks must be visible there with their
  // container indices. (The demuxer-side GetStreams() override is exercised
  // by the switch tests below: without it, SelectAudioTrack finds no
  // alternate and fails.)
  const MediaInfo info = pipeline_->media_info();
  const std::vector<const StreamInfo*> audio =
      info.StreamsOfKind(StreamKind::kAudio);
  ASSERT_EQ(audio.size(), 2u);
  EXPECT_EQ(audio[0]->index, kTrackA);
  EXPECT_EQ(audio[1]->index, kTrackB);
}

// Regression anchor for the pre-switch state: an audio-only container with
// two tracks must play to EOS with no track switch at all (the demuxer now
// routes only the active stream; a wedged demux loop shows up here first).
TEST_F(PipelineTrackTest, PlaysAudioOnlyTwoTrackFileToEosWithoutSwitch) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));
  if (audio_sinks_->last_sink()) {
    audio_sinks_->last_sink()->set_render_runner(audio_thread_.task_runner());
  }
  pipeline_->Play();
  ASSERT_TRUE(PumpUntil([this] { return client_.ended(); }))
      << "no-switch playback did not reach EOS; events:\n"
      << client_.EventLog();
}

TEST_F(PipelineTrackTest, SwitchAudioTrackMidPlayAndReachEos) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }))
      << "never probed the source; events:\n"
      << client_.EventLog();
  // Serialize test-thread pulls onto the renderer's own sequence; without
  // this the direct PullPeriod path races the renderer teardown (the same
  // arm-the-sink step the seek and throttle fixtures do).
  if (audio_sinks_->last_sink()) {
    audio_sinks_->last_sink()->set_render_runner(audio_thread_.task_runner());
  }
  pipeline_->Play();
  // No HaveEnough gate here: the buffering verdict only fires on a
  // DRY->RECOVER edge, and a fixture that pumps faster than realtime never
  // goes dry. Playing is proven by reaching EOS below.

  std::atomic<bool> switched{false};
  PipelineStatus switch_status = PipelineStatus::kTrackSwitchError;
  pipeline_->SelectAudioTrack(
      kTrackB,
      base::BindOnce(
          [](std::atomic<bool>* flag, PipelineStatus* out, PipelineStatus s) {
            *out = s;
            flag->store(true);
          },
          &switched, &switch_status));
  ASSERT_TRUE(PumpUntil([&] { return switched.load(); }))
      << "track switch never completed; events:\n"
      << client_.EventLog();
  EXPECT_EQ(switch_status, PipelineStatus::kOk);
  EXPECT_FALSE(client_.error()) << client_.error().ToString();

  // Playback continues on the new track and reaches the end of the 3 s file.
  ASSERT_TRUE(PumpUntil([this] { return client_.ended(); }))
      << "playback did not reach EOS after the switch; events:\n"
      << client_.EventLog();
}

TEST_F(PipelineTrackTest, SwitchingBackToTheFirstTrackAlsoWorks) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));
  if (audio_sinks_->last_sink()) {
    audio_sinks_->last_sink()->set_render_runner(audio_thread_.task_runner());
  }
  pipeline_->Play();

  for (const int target : {kTrackB, kTrackA, kTrackB}) {
    std::atomic<bool> switched{false};
    PipelineStatus status = PipelineStatus::kTrackSwitchError;
    pipeline_->SelectAudioTrack(
        target,
        base::BindOnce(
            [](std::atomic<bool>* flag, PipelineStatus* out, PipelineStatus s) {
              *out = s;
              flag->store(true);
            },
            &switched, &status));
    ASSERT_TRUE(PumpUntil([&] { return switched.load(); }))
        << "switch to " << target << " never completed";
    EXPECT_EQ(status, PipelineStatus::kOk);
    EXPECT_FALSE(client_.error());
  }
}

TEST_F(PipelineTrackTest, UnknownStreamIndexFailsWithoutErrorState) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));

  std::atomic<bool> done{false};
  PipelineStatus status = PipelineStatus::kOk;
  pipeline_->SelectAudioTrack(
      99,
      base::BindOnce(
          [](std::atomic<bool>* flag, PipelineStatus* out, PipelineStatus s) {
            *out = s;
            flag->store(true);
          },
          &done, &status));
  ASSERT_TRUE(PumpUntil([&] { return done.load(); }));
  EXPECT_EQ(status, PipelineStatus::kTrackSwitchError);
  // A rejected switch is not a playback failure: the pipeline stays healthy.
  EXPECT_FALSE(client_.error());
}

}  // namespace
}  // namespace avbase::media
