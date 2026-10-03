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
#include "media/base/timed_text.h"
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

// Phase 4.2 text leg: the base renders nothing -- subtitle packets are
// decoded on S1 and delivered as TimedTextCues through Client::OnTimedText.
// The media is an audio file with TWO subrip tracks
// (tests/testdata/audio_two_subs.mkv, streams 1=eng and 2=chi), so selection
// has a real alternate and the delivery order is deterministic.
class PipelineTextTest : public PipelineTestFixture {
 protected:
  PipelineTextTest() : PipelineTestFixture(/*ffmpeg_mode=*/true) {
    media_file_ = "audio_two_subs.mkv";
    with_text_factory_ = true;
  }

  // The container stream indices of the two subrip tracks (streams 1=eng,
  // 2=chi; stream 0 is the audio track).
  static constexpr int kTrackEng = 1;
  static constexpr int kTrackChi = 2;

  void SelectTextTrack(int index, PipelineStatus* out_status,
                       std::atomic<bool>* done) {
    pipeline_->SelectTextTrack(
        index,
        base::BindOnce(
            [](std::atomic<bool>* flag, PipelineStatus* out, PipelineStatus s) {
              *out = s;
              flag->store(true);
            },
            done, out_status));
  }
};

TEST_F(PipelineTextTest, NoCuesUntilATrackIsSelected) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }))
      << "never probed the source; events:\n"
      << client_.EventLog();
  pipeline_->Play();
  // The text leg is off by default: no track selected, no cues, and the
  // pipeline still reaches EOS on the audio track.
  ASSERT_TRUE(PumpUntil([this] { return client_.ended(); }))
      << "audio-only playback did not reach EOS; events:\n"
      << client_.EventLog();
  EXPECT_TRUE(client_.cues().empty());
  EXPECT_FALSE(client_.error());
}

TEST_F(PipelineTextTest, SelectedTrackDeliversItsCues) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));
  pipeline_->Play();

  std::atomic<bool> done{false};
  PipelineStatus status = PipelineStatus::kTrackSwitchError;
  SelectTextTrack(kTrackEng, &status, &done);
  ASSERT_TRUE(PumpUntil([&] { return done.load(); }))
      << "text selection never completed; events:\n"
      << client_.EventLog();
  EXPECT_EQ(status, PipelineStatus::kOk);

  ASSERT_TRUE(PumpUntil([this] { return client_.cues().size() >= 3; }))
      << "no cues delivered; events:\n"
      << client_.EventLog();
  // The English track's words, in order. Content equality (not count) is the
  // assertion: it proves both WHICH track was decoded and that the decoder
  // flattened the rects into text.
  ASSERT_EQ(client_.cues().size(), 3u);
  EXPECT_NE(client_.cues()[0].text.find("first track alpha"),
            std::string::npos);
  EXPECT_NE(client_.cues()[1].text.find("first track beta"), std::string::npos);
  EXPECT_NE(client_.cues()[2].text.find("first track gamma"),
            std::string::npos);
  // Cue timing came from the container (0.1s, 0.7s, 1.3s starts).
  EXPECT_EQ(client_.cues()[0].pts, base::Milliseconds(100));
  EXPECT_EQ(client_.cues()[0].duration, base::Milliseconds(500));
  EXPECT_FALSE(client_.error());
}

TEST_F(PipelineTextTest, SwitchingToTheSecondTrackDeliversItsCues) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));
  pipeline_->Play();

  std::atomic<bool> done{false};
  PipelineStatus status = PipelineStatus::kTrackSwitchError;
  SelectTextTrack(kTrackEng, &status, &done);
  ASSERT_TRUE(PumpUntil([&] { return done.load(); }));
  ASSERT_TRUE(PumpUntil([this] { return client_.cues().size() >= 3; }));

  done.store(false);
  SelectTextTrack(kTrackChi, &status, &done);
  ASSERT_TRUE(PumpUntil([&] { return done.load(); }))
      << "second selection never completed; events:\n"
      << client_.EventLog();
  EXPECT_EQ(status, PipelineStatus::kOk);
  ASSERT_TRUE(PumpUntil([this] {
    for (const TimedTextCue& cue : client_.cues()) {
      if (cue.text.find("second track alpha") != std::string::npos) {
        return true;
      }
    }
    return false;
  })) << "the second track's cues never arrived; events:\n"
      << client_.EventLog();
  EXPECT_FALSE(client_.error());
}

TEST_F(PipelineTextTest, UnknownTextTrackFailsCleanly) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.HaveMetadata(); }));
  pipeline_->Play();

  std::atomic<bool> done{false};
  PipelineStatus status = PipelineStatus::kOk;
  SelectTextTrack(42, &status, &done);
  ASSERT_TRUE(PumpUntil([&] { return done.load(); }));
  EXPECT_EQ(status, PipelineStatus::kTrackSwitchError);
  EXPECT_TRUE(client_.cues().empty());
  EXPECT_FALSE(client_.error());
}

}  // namespace
}  // namespace avbase::media
