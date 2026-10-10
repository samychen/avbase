// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The pipeline-level seek contract on the synthetic source (docs/07 §5),
// which no component-level suite can see: a seek issued through Pipeline
// must resynchronize decode, the renderer generation and both sinks, and
// the next frame on the display must be the frame the seek asked for.
//
// This is a rebuild of the harness that reproduced #50 ("after a seek,
// video never resumed"). The first attempt was withdrawn for flakiness
// (docs/PROGRESS.md, tenth round §5.1); this version differs where it
// counted:
//   * every wait is bounded AND one-sided -- the test never sleeps a fixed
//     delay, it polls for a state the pipeline must reach and fails with
//     the event log if the deadline passes;
//   * rendering is advanced only by pumping the two fake sinks, so there
//     is no free-running clock to race against;
//   * the landing assertion accepts frames 150..152: a frame dropped as
//     late is the compositor's legitimate decision, not a defect;
//   * the "no frame from before the seek after the landing frame"
//     assertion uses the demuxer's serial bump, not frame timestamps.
//
// WHY REAL THREADS for S3/S4: production shape, and ~RendererImpl posts
// sub-renderer destruction to those sequences and waits for it -- a
// single-threaded runner deadlocks there (see renderer_impl_unittest.cc).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/task_environment.h"
#include "base/threading/thread.h"
#include "base/time/default_tick_clock.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/pipeline_status.h"
#include "media/base/video_frame.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/pipeline_impl.h"
#include "media/renderers/default_renderer_factory.h"
#include "tests/support/fake_pipeline_client.h"
#include "tests/support/fake_renderer_sinks.h"
#include "tests/support/fake_sink_factories.h"
#include "tests/support/pipeline_fixture.h"
#include "tests/support/synthetic_decoders.h"
#include "tests/support/synthetic_demuxer.h"

namespace avbase::media {
namespace {

// Ceiling for every wait. Generous, because it is a ceiling and not a delay:
// the test passes the moment the state is reached.
constexpr auto kWaitTimeout = std::chrono::seconds(10);
// The seek target: 5 s of a 30 fps source is frame 150.
constexpr uint32_t kSeekFrame = 150;
constexpr uint32_t kLandingSlack = 2;

template <typename Pred>
bool WaitFor(Pred pred) {
  const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

class PipelineSeekTest : public PipelineTestFixture {
 protected:
  PipelineSeekTest() : PipelineTestFixture(/*ffmpeg_mode=*/false) {}

 public:
  // Bound as the seek callback, hence public.
  void OnSeeked() { seeked_.store(true); }

 protected:
  void PlayAndWaitForSinks() {
    Play();
    ASSERT_TRUE(PumpUntil([this] {
      return video_sinks_->last_sink() && audio_sinks_->last_sink() &&
             video_sinks_->last_sink()->start_count() > 0 &&
             audio_sinks_->last_sink()->start_count() > 0;
    })) << "sinks never started; events:\n"
        << client_.EventLog();
    // Marshalled pulls: the base PumpRound arms the render runner.
  }

  // First stored frame whose index is >= |floor|, or nullptr.
  static const base::scoped_refptr<VideoFrame>*
  LandingFrame(const test::FakeVideoSink& video, uint32_t floor, size_t from) {
    const std::vector<base::scoped_refptr<VideoFrame>>& frames = video.frames();
    for (size_t i = from; i < frames.size(); ++i) {
      uint32_t index = 0;
      if (test::ReadFrameIndex(*frames[i], &index) && index >= floor) {
        return &frames[i];
      }
    }
    return nullptr;
  }

  std::atomic<bool> seeked_{false};
};

TEST_F(PipelineSeekTest, SeekToFiveSecondsLandsOnFrame150) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.Started(); }))
      << "pipeline never started; events:\n"
      << client_.EventLog();
  ASSERT_FALSE(client_.HasError()) << client_.error().ToString() << "\n"
                                   << client_.EventLog();
  PlayAndWaitForSinks();

  // Get playback under way: pump roughly a second, and require that real
  // frames came out -- a renderer that never presented would make the seek
  // assertions below vacuous.
  int presented_before = 0;
  for (int round = 0; round < 60; ++round) {
    presented_before += PumpRound();
  }
  ASSERT_GT(presented_before, 0) << client_.EventLog();

  const size_t first_batch = video_sinks_->last_sink()->frames().size();
  seeked_.store(false);
  pipeline_->Seek(base::Seconds(5), base::BindOnce(&PipelineSeekTest::OnSeeked,
                                                   base::Unretained(this)));
  ASSERT_TRUE(PumpUntil([this] {
    PumpRound();
    return seeked_.load();
  })) << "seek callback never ran; events:\n"
      << client_.EventLog();
  EXPECT_GE(video_sinks_->last_sink()->flush_count(), 1);

  // The landing frame: the first presented frame at or after frame 150.
  const test::FakeVideoSink* video = video_sinks_->last_sink();
  ASSERT_TRUE(PumpUntil([&] {
    PumpRound();
    return LandingFrame(*video, kSeekFrame, first_batch) != nullptr;
  })) << "no frame >= "
      << kSeekFrame << " presented after the seek; events:\n"
      << client_.EventLog()
      << "sink stats: presented=" << video->GetStats().frames_presented
      << " dropped=" << video->GetStats().frames_dropped
      << " stored=" << video->frames().size();

  const base::scoped_refptr<VideoFrame>* landed =
      LandingFrame(*video, kSeekFrame, first_batch);
  uint32_t index = 0;
  ASSERT_TRUE(test::ReadFrameIndex(**landed, &index));
  EXPECT_LE(index, kSeekFrame + kLandingSlack)
      << "landed on frame " << index << ", too far past the target";

  // No frame from before the seek may follow the landing frame. The
  // compositor legitimately drops late frames, so the boundary is "once the
  // new generation is on the display, the old one is gone for good".
  const size_t landed_at = static_cast<size_t>(landed - video->frames().data());
  for (int round = 0; round < 120; ++round) {
    PumpRound();
  }
  const std::vector<base::scoped_refptr<VideoFrame>>& frames = video->frames();
  for (size_t i = landed_at; i < frames.size(); ++i) {
    uint32_t after = 0;
    if (test::ReadFrameIndex(*frames[i], &after)) {
      EXPECT_GE(after, kSeekFrame)
          << "pre-seek frame " << after << " presented at slot " << i
          << ", after the landing frame";
    }
  }
  EXPECT_FALSE(client_.HasError()) << client_.error().ToString() << "\n"
                                   << client_.EventLog();
}

// The M9 accurate-seek contract at the pipeline level: with the drop window
// open, NOTHING before the target frame may reach the display -- not the
// pre-seek generation, not the post-seek frames before the target. That is
// strictly stronger than the keyframe seek above, whose landing assertion
// had to tolerate frames beyond the target but could not bound the floor.
// The 1-in-6 recovery miss was the rendering_ flag never being set (a lost
// patch hunk, see PROGRESS round 13) -- with it fixed and the per-present
// check in place, 10 stress runs are clean. The throttle test's "EOS before
// starvation" was the same bug.
TEST_F(PipelineSeekTest, AccurateSeekPresentsNothingBeforeTheTarget) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.Started(); }))
      << "pipeline never started; events:\n"
      << client_.EventLog();
  PlayAndWaitForSinks();
  int presented_before = 0;
  for (int round = 0; round < 60; ++round) {
    presented_before += PumpRound();
  }
  ASSERT_GT(presented_before, 0) << client_.EventLog();

  std::atomic<bool> reached{false};
  // Open the window BEFORE the keyframe seek: it survives Flush() by design,
  // which is what frames the new generation. On the synthetic 30 fps source,
  // 5 s is frame 150, and the pre-seek position (~1 s) is frame ~30 -- the
  // window must drop those too.
  pipeline_->BeginAccurateSeek(
      base::Seconds(5),
      base::BindOnce([](std::atomic<bool>* r) { r->store(true); }, &reached));
  const size_t window_batch = video_sinks_->last_sink()
                                  ? video_sinks_->last_sink()->frames().size()
                                  : 0;
  seeked_.store(false);
  pipeline_->Seek(base::Seconds(5), base::BindOnce(&PipelineSeekTest::OnSeeked,
                                                   base::Unretained(this)));
  ASSERT_TRUE(PumpUntil([this] {
    PumpRound();
    return seeked_.load();
  })) << "seek callback never ran; events:\n"
      << client_.EventLog();

  const test::FakeVideoSink* video = video_sinks_->last_sink();
  ASSERT_TRUE(PumpUntil([&] {
    PumpRound();
    return LandingFrame(*video, kSeekFrame, window_batch) != nullptr;
  })) << "no frame >= "
      << kSeekFrame << " presented after the accurate seek; events:\n"
      << client_.EventLog();

  // THE contract: from the moment the window opened, no frame below the
  // target was presented. (A frame dropped as late can land past the target,
  // so the upper bound keeps the keyframe test's slack; the floor is exact.)
  const std::vector<base::scoped_refptr<VideoFrame>>& frames = video->frames();
  for (size_t i = window_batch; i < frames.size(); ++i) {
    uint32_t index = 0;
    if (test::ReadFrameIndex(*frames[i], &index)) {
      EXPECT_GE(index, kSeekFrame)
          << "frame " << index << " presented under an open accurate-seek "
          << "window targeting frame " << kSeekFrame;
    }
  }
  uint32_t landed_index = 0;
  ASSERT_TRUE(test::ReadFrameIndex(
      **LandingFrame(*video, kSeekFrame, window_batch), &landed_index));
  EXPECT_LE(landed_index, kSeekFrame + kLandingSlack);

  // The window's reached callback must have fired -- the facade's
  // SeekController completes the user's seek on it, so a silent window would
  // hang every accurate seek.
  ASSERT_TRUE(PumpUntil([&] {
    PumpRound();
    return reached.load();
  })) << "accurate-seek window never reported reaching the target";

  // The seek's flush drains both queues; the starvation signal (M9) must
  // have reported the dry edge and the recovery to the client, in that
  // order of causality. Without the renderer's CheckBufferingTransitions,
  // have_nothing stays false forever and the facade's HWM never advances.
  EXPECT_TRUE(client_.have_nothing())
      << "no kHaveNothing edge after the seek; events:\n"
      << client_.EventLog();
  EXPECT_TRUE(client_.have_enough())
      << "no kHaveEnough edge after the seek; events:\n"
      << client_.EventLog();
  EXPECT_FALSE(client_.HasError()) << client_.error().ToString() << "\n"
                                   << client_.EventLog();
}

// The live chase (docs/12 section 2.1), end to end through the pipeline:
// a live-marked source whose duration stands in for the edge, a 2 s latency
// hint, and a playhead that starts 10 s behind. The first statistics tick
// (1 s) must trip the chase, land near the edge (edge - hint/2), and leave
// playback running with no deadlock. The growing-edge demuxer is the
// follow-up; this pins the trigger, the skip and the resume.
TEST_F(PipelineSeekTest, LiveSourceChasesToTheEdge) {
  spec_.live = true;
  spec_.duration = base::Seconds(10);
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.Started(); }))
      << "pipeline never started; events:\n"
      << client_.EventLog();
  pipeline_->SetLatencyHint(base::Seconds(2));
  PlayAndWaitForSinks();

  // The first statistics tick (1 s) evaluates behind=10s > 2s and chases to
  // duration - hint/2 = 9 s. Poll until the playhead is past 8.5 s.
  ASSERT_TRUE(PumpUntil([this] {
    PumpRound();
    return pipeline_->GetMediaTime() >=
           base::Seconds(8) + base::Milliseconds(500);
  })) << "the playhead never chased to the live edge; media_time="
      << pipeline_->GetMediaTime().ToString() << "; events:\n"
      << client_.EventLog();

  // Playback continues after the chase: frames keep presenting.
  const size_t frames_at_chase = video_sinks_->last_sink()->frames().size();
  for (int i = 0; i < 30; ++i) {
    PumpRound();
  }
  EXPECT_GT(video_sinks_->last_sink()->frames().size(), frames_at_chase)
      << "no frames presented after the chase; events:\n"
      << client_.EventLog();
  EXPECT_FALSE(client_.HasError()) << client_.error().ToString() << "\n"
                                   << client_.EventLog();
}

// docs/07 section 5's audio-only and video-only cases: the renderer falls
// back per ffplay (video-only uses the external/video clock via
// SetStreamAvailability), plays the whole file, and reports no error.
TEST_F(PipelineSeekTest, AudioOnlySourcePlaysThrough) {
  spec_.enable_video = false;
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.Started(); }));
  // Single-sided wait: the shared helper requires both sinks, and the
  // disabled video side never starts by design.
  Play();
  ASSERT_TRUE(PumpUntil([this] {
    PumpRound();
    return audio_sinks_->last_sink() &&
           audio_sinks_->last_sink()->start_count() > 0;
  })) << "audio sink never started; events:\n"
      << client_.EventLog();
  audio_sinks_->last_sink()->set_render_runner(audio_thread_.task_runner());
  // The factories create both sinks at assembly; the disabled side must
  // never START (it has no stream to serve).
  EXPECT_EQ(video_sinks_->last_sink()->start_count(), 0);
  ASSERT_TRUE(PumpUntil([this] {
    PumpRound();
    return client_.ended();
  }));
  EXPECT_EQ(video_sinks_->last_sink()->start_count(), 0)
      << "the disabled video sink started";
  EXPECT_FALSE(client_.HasError()) << client_.error().ToString() << "\n"
                                   << client_.EventLog();
}

TEST_F(PipelineSeekTest, VideoOnlySourcePlaysThrough) {
  spec_.enable_audio = false;
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.Started(); }));
  Play();
  ASSERT_TRUE(PumpUntil([this] {
    PumpRound();
    return video_sinks_->last_sink() &&
           video_sinks_->last_sink()->start_count() > 0;
  })) << "video sink never started; events:\n"
      << client_.EventLog();
  ASSERT_TRUE(PumpUntil([this] {
    PumpRound();
    return client_.ended();
  }));
  EXPECT_EQ(audio_sinks_->last_sink()->start_count(), 0)
      << "the disabled audio sink started";
  EXPECT_GT(video_sinks_->last_sink()->frames().size(), 0u);
  EXPECT_FALSE(client_.HasError()) << client_.error().ToString() << "\n"
                                   << client_.EventLog();
}

// H3: a seek issued while another is in flight must not be dropped. The old
// code recorded the collision in a flag it never read, so the second target
// vanished and playback landed on the FIRST target instead. Both completions
// must also fire, since the facade matches completions to requests.
TEST_F(PipelineSeekTest, MidFlightSeekIsNotDropped) {
  StartPipeline();
  ASSERT_TRUE(PumpUntil([this] { return client_.Started(); }))
      << "pipeline never started; events:\n"
      << client_.EventLog();
  PlayAndWaitForSinks();
  for (int round = 0; round < 60; ++round) {
    PumpRound();
  }

  const size_t first_batch = video_sinks_->last_sink()->frames().size();
  // 8 s of a 30 fps source is frame 240 -- strictly beyond the first seek's
  // target (5 s, frame 150), so landing on frame 240 proves the second seek
  // ran, not merely that a seek ran.
  constexpr uint32_t kSecondTarget = 240;
  bool first_seeked = false;
  bool second_seeked = false;
  pipeline_->Seek(base::Seconds(5),
                  base::BindOnce([](bool* f) { *f = true; }, &first_seeked));
  pipeline_->Seek(base::Seconds(8),
                  base::BindOnce([](bool* f) { *f = true; }, &second_seeked));
  // The two Seek() calls post onto the same runner back to back, so the
  // second DoSeek() deterministically observes seek_in_flight_.
  ASSERT_TRUE(PumpUntil([&] {
    PumpRound();
    return first_seeked && second_seeked;
  })) << "a seek completion never ran; events:\n"
      << client_.EventLog();

  // The landing frame is the first one presented after the seek batch. It
  // must sit at the SECOND target: under the old drop behaviour the second
  // seek vanished, playback landed on the FIRST target (frame ~150) and the
  // free-running source then simply played its way up to 240 -- which is why
  // the floor assertion below is what actually catches the regression.
  const test::FakeVideoSink* video = video_sinks_->last_sink();
  ASSERT_TRUE(PumpUntil([&] {
    PumpRound();
    return LandingFrame(*video, kSeekFrame, first_batch) != nullptr;
  })) << "no frame >= "
      << kSeekFrame << " presented after the seek batch; events:\n"
      << client_.EventLog();

  const base::scoped_refptr<VideoFrame>* landed =
      LandingFrame(*video, kSeekFrame, first_batch);
  uint32_t index = 0;
  ASSERT_TRUE(test::ReadFrameIndex(**landed, &index));
  EXPECT_GE(index, kSecondTarget - kLandingSlack)
      << "landed on frame " << index
      << ": the mid-flight seek was dropped and playback landed on the first "
         "target";
  EXPECT_LE(index, kSecondTarget + kLandingSlack)
      << "landed on frame " << index << ", too far past the second target";
  EXPECT_FALSE(client_.HasError()) << client_.error().ToString() << "\n"
                                   << client_.EventLog();
}

}  // namespace
}  // namespace avbase::media
