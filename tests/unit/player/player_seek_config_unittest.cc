// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// config.seek.accurate ("enable-accurate-seek") as the veto over
// SeekMode::kAccurate.
//
// WHY THIS EXISTS. The field existed, docs/05 Δ19 described it, the option
// registry used to accept the key -- and nothing read it. SeekTo decided
// accuracy from the per-call SeekMode alone, so "enable-accurate-seek=0" (an
// ijkplayer call site expecting keyframe landings) produced accurate seeks
// anyway. The reading is now in PlayerImpl::SeekTo, and the assertions below
// are what keeps it there: an option that is read but not asserted is one edit
// away from being the dead field it just stopped being.
//
// BOTH DIRECTIONS ARE PINNED, and the second one is why this file can be
// deterministic. The accurate-seek state machine always ends by posting
// kAccurateSeekCompleted -- whether the target was reached or the timeout
// expired (player_impl_accurate_seek.cc, CompleteAccurateSeek) -- so "the
// machine ran at all" is observable without depending on a decoder winning a
// race. What is deliberately NOT asserted in the allowed case is the seek's
// success: a prepared-but-never-presented pipeline may legitimately expire.

#include "player/player_impl.h"

#include <atomic>
#include <memory>
#include <string>

#include "avbase/BuildConfig.h"
#include "base/functional/bind.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "player/public/player.h"
#include "player/public/player_config.h"
#include "player/public/player_event.h"

#if !AVBASE_ENABLE_FFMPEG
// Seeking needs a demuxer, so without FFmpeg there is no seek to gate; the
// suite compiles to nothing rather than registering tests that would skip
// everywhere (same reasoning as prepare_autostart_unittest.cc).
#else

namespace avbase {
namespace {

#ifndef AVBASE_TESTDATA_DIR
#error "player_unittests must define AVBASE_TESTDATA_DIR for this suite"
#endif
// The short fixture, deliberately. A 3 s file reaches EOF almost immediately,
// which is the state a seek lands in whenever playback has run to the end --
// and that state used to swallow the seek whole (docs/04 §2.1). It is now
// fixed, so this fixture doubles as the regression net for it: with
// FFmpegDemuxer::DemuxLoop() parking at EOF instead of returning, 200
// consecutive runs of this suite are clean (they were 3/120 red before).
const char* const kLocalUri = AVBASE_TESTDATA_DIR "/small_h264_aac_3s.mp4";

// What the event stream and the callback reported. The handler runs on the
// player's event sequence, so these outlive it and are read after the wait.
struct Probe {
  std::atomic<int> accurate_completed{0};
  std::atomic<int> seek_completed{0};
  std::atomic<int> errors{0};
  base::WaitableEvent callback_finished;
  std::atomic<bool> callback_ok{false};
};

void OnEvent(Probe* probe, const PlayerEvent& event) {
  switch (event.type) {
  case EventType::kAccurateSeekCompleted:
    probe->accurate_completed.fetch_add(1);
    break;
  case EventType::kSeekCompleted:
    probe->seek_completed.fetch_add(1);
    break;
  case EventType::kError:
    probe->errors.fetch_add(1);
    break;
  default:
    break;
  }
}

void OnSeekDone(Probe* probe, Status status) {
  probe->callback_ok.store(status.has_value());
  probe->callback_finished.Signal();
}

// Prepares WITHOUT autostart and seeks from kPrepared, then returns with
// |probe| filled in. Skips (rather than fails) when the container is missing,
// which is the same contract the neighbouring player suites use.
//
// WHY THE PAUSED SHAPE: this file is about which path a seek takes, so the
// seek should be the only thing in flight. Autostart would put a start-up
// burst of decode work in the same window and make a failure ambiguous --
// "the veto is broken" and "playback was still settling" would look the same.
// Seeking from kPrepared is a documented state for SeekTo, and it exercises
// the same DoSeek path.
//
// THE EOF WINDOW (why this helper's history matters). With the 3 s fixture it
// used to hang in 3 of 120 runs: the seek's renderer half completed,
// OnSeekDemuxerDone never fired, and a stack sample showed avbase-media AND
// avbase-demux both idle -- nothing was blocked, something was simply never
// done. The cause was in the demuxer, not here: FFmpegDemuxer::DemuxLoop()
// broke out of its loop at AVERROR_EOF while StartPlayingFrom() only
// publishes pending_seek_ and signals resume_event_, so a seek arriving after
// the loop had returned was never picked up and its callback sat in
// pending_seek_cbs_ forever. The loop now parks at EOF ("kEof") and waits for
// the same signal, and the deterministic red test for it lives in
// ffmpeg_demuxer_unittest.cc (SeekAfterEndOfStreamIsAnsweredAndResumesReads)
// -- staying here would mean betting on the race instead of pinning it.
void SeekAccurately(PlayerConfig config, base::TimeDelta position,
                    Probe* probe) {
  config.start_on_prepared = false;
  PlayerImpl impl(config, nullptr);
  impl.SetEventHandler(base::BindRepeating(&OnEvent, base::Unretained(probe)));
  const Status configured = impl.SetDataSource(kLocalUri);
  if (!configured.has_value()) {
    GTEST_SKIP() << "test media unavailable: " << configured.error().ToString();
  }
  ASSERT_TRUE(impl.PrepareSync(base::Seconds(30)).has_value());
  ASSERT_EQ(impl.state(), PlayerState::kPrepared);

  const auto id =
      impl.SeekTo(position, SeekMode::kAccurate,
                  base::BindOnce(&OnSeekDone, base::Unretained(probe)));
  ASSERT_TRUE(id.has_value()) << id.error().ToString();
  ASSERT_TRUE(probe->callback_finished.TimedWait(base::Seconds(10)))
      << "neither the accurate-seek path nor the plain one completed the "
         "request";
  impl.StopSync(base::Milliseconds(500));
}

TEST(PlayerSeekConfigTest, VetoedAccurateSeekCompletesAsAPlainSeek) {
  PlayerConfig config;
  config.seek.accurate = false;
  Probe probe;
  SeekAccurately(config, base::Seconds(1), &probe);

  // The veto bypassed the state machine entirely, so the caller sees exactly
  // what kPreviousKeyframe would have given it.
  EXPECT_EQ(probe.accurate_completed.load(), 0)
      << "the accurate-seek machine ran while config.seek.accurate was false";
  EXPECT_EQ(probe.seek_completed.load(), 1);
  EXPECT_TRUE(probe.callback_ok.load())
      << "a vetoed accurate seek is a normal seek: it must not be reported as "
         "a failure";
  EXPECT_EQ(probe.errors.load(), 0);
}

TEST(PlayerSeekConfigTest, AllowedAccurateSeekRunsTheStateMachine) {
  PlayerConfig config;
  config.seek.accurate = true;  // the default, stated so the two tests read as
                                // the pair they are
  // Nothing is presenting in this shape, so the window ends by expiry and the
  // 5 s default would be paid in full by every run. 250 ms is the same event
  // on a test-sized clock: what is asserted is that the machine ran and
  // announced itself, not how long it waited.
  config.seek.accurate_timeout = base::Milliseconds(250);
  Probe probe;
  SeekAccurately(config, base::Seconds(1), &probe);

  // Reached or expired, the machine always announces itself -- see this file's
  // header on why that makes the contrast deterministic.
  EXPECT_EQ(probe.accurate_completed.load(), 1)
      << "kAccurate stopped opening the accurate-seek window; the option is "
         "back to being unread or the gate is stuck shut";
  EXPECT_EQ(probe.seek_completed.load(), 1)
      << "every seek reports exactly one completion, whichever path it took";
}

}  // namespace
}  // namespace avbase

#endif  // !AVBASE_ENABLE_FFMPEG
