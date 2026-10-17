// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// What PrepareSync() returns once the autostart has been sequenced ahead of
// its wakeup.
//
// WHY THIS EXISTS. PlayerImpl::OnPipelineReady used to do, in this order:
//     prepared_event_.Signal();
//     if (config_.start_on_prepared) Start();
// Both on the media sequence. PrepareSync() returns on that signal, so it
// could be awake and reading state() BEFORE Start() had run -- the wakeup
// raced the autostart it had been promised had already finished. A comment in
// player_impl_prepare.cc claimed the opposite ("Start() already ran by the
// time the event fired"); it was wrong about its own code, and three call
// sites each rediscovered the consequence: writing Start() after
// PrepareSync() is the ILLEGAL call, because the autostart may have beaten
// you to kStarted.
//
// The order is now Start() then Signal(), so the wakeup cannot lose. That is
// a behaviour change to the prepare path, and this suite pins the CONTRACT
// callers depend on rather than the ordering itself.
//
// NOTE ON WHAT THESE TESTS CANNOT DO. Reverting the order does not turn them
// red -- measured, not predicted. Signal() holds the WaitableEvent mutex and
// the waiter needs that same mutex, so the autostart had already won in every
// one of 20 iterations even with the buggy order. See the comment on the
// assertion for the full reasoning. The ordering is therefore carried by the
// comment in OnPipelineReady, not by a test; anyone who needs it pinned for
// real needs a hook in the prepare path, which is not worth adding for this.

#include "player/player_impl.h"

#include <memory>
#include <string>

#include "avbase/BuildConfig.h"
#include "gtest/gtest.h"
#include "player/public/player_config.h"

#if !AVBASE_ENABLE_FFMPEG
// Same reason as player_prepare_teardown_unittest.cc: preparing needs a
// demuxer, so without FFmpeg there is no autostart to race. Compiles to
// nothing rather than registering tests that would have to skip.
#else

namespace avbase {
namespace {

// Opens successfully so OnPipelineReady actually runs; the file is tiny and
// fully local, and nothing here decodes or presents a frame. The path comes
// from AVBASE_TESTDATA_DIR because ctest runs from the build tree, where a
// relative "tests/testdata/..." resolves to nothing -- and a test that
// depends on its working directory passes under ./bin/foo and fails under
// ctest, which is the worst possible place to learn that.
#ifndef AVBASE_TESTDATA_DIR
#error "player_unittests must define AVBASE_TESTDATA_DIR for this suite"
#endif
const char* const kLocalUri = AVBASE_TESTDATA_DIR "/small_h264_aac_3s.mp4";

TEST(PrepareAutostartTest, PrepareSyncSucceedsWhicheverSideOfTheRaceItWakes) {
  int saw_prepared = 0;
  int saw_started = 0;
  for (int i = 0; i < 20; ++i) {
    PlayerConfig config;
    config.start_on_prepared = true;  // the default; stated so the test does
                                      // not silently change meaning if it does
    PlayerImpl impl(config, nullptr);
    const Status configured = impl.SetDataSource(kLocalUri);
    if (!configured.has_value()) {
      GTEST_SKIP() << "test media unavailable: "
                   << configured.error().ToString();
    }
    const Status prepared = impl.PrepareSync(base::Seconds(30));
    ASSERT_TRUE(prepared.has_value()) << "iteration " << i;

    // The invariant, not the exact value: these three are precisely the
    // states PrepareSync() accepts, so seeing any other one would mean it
    // returns OkStatus for a state it rejects a moment later.
    //
    // WHY NOT EXPECT_EQ(kStarted), which the new ordering would satisfy
    // trivially: measured, this loop never saw kPrepared even WITH the signal
    // first. Signal() runs on the media sequence holding the WaitableEvent
    // mutex, and the waiter cannot return from TimedWait() until it takes that
    // same mutex -- so the wakeup is already serialized behind the autostart,
    // and Start() (a few instructions under state_lock_) wins essentially
    // always. A reverse-ordering test does NOT go red when the order is
    // reverted: tried it, it stayed green. Asserting kStarted here would be
    // asserting a scheduling accident.
    //
    // So what this file actually pins is the CONTRACT, which is what callers
    // depend on: PrepareSync() succeeds, the state it hands back is one it
    // accepts, and an unguarded Start() on top of it logs and changes
    // nothing. The ordering itself is pinned by the comment in
    // OnPipelineReady -- a code comment cannot fail a build, and pretending
    // otherwise in a test comment is how the next person trusts a test that
    // proves nothing.
    const PlayerState s = impl.state();
    EXPECT_TRUE(s == PlayerState::kPrepared || s == PlayerState::kStarted ||
                s == PlayerState::kCompleted)
        << "iteration " << i << " woke in state " << GetPlayerStateName(s);
    if (s == PlayerState::kPrepared) {
      ++saw_prepared;
    } else if (s == PlayerState::kStarted) {
      ++saw_started;
    }

    // What a caller is actually tempted to write next, and what it does now
    // that Start() precedes the signal: the autostart has already run, so this
    // is the rejected call. Guarded in examples/pull_frames.cc precisely
    // because it is. Calling it here is deliberate -- a caller that forgets
    // the guard must get a log line and no state change, never a crash or a
    // second Play().
    impl.Start();
    EXPECT_EQ(impl.state(), PlayerState::kStarted);

    impl.StopSync(base::Milliseconds(500));
  }
  // Not an assertion about the ordering -- only that the loop really ran and
  // every iteration reached a verdict. A test that can pass without ever
  // preparing is the same false-green shape this file's neighbours fight,
  // and 20 iterations is here to catch a wakeup that happens to land badly
  // rather than to prove the autostart runs.
  EXPECT_EQ(saw_prepared + saw_started, 20);
}

TEST(PrepareAutostartTest,
     StartOnPreparedFalseLeavesTheCallerResponsibleForStarting) {
  PlayerConfig config;
  config.start_on_prepared = false;
  PlayerImpl impl(config, nullptr);
  const Status configured = impl.SetDataSource(kLocalUri);
  if (!configured.has_value()) {
    GTEST_SKIP() << "test media unavailable: " << configured.error().ToString();
  }
  ASSERT_TRUE(impl.PrepareSync(base::Seconds(30)).has_value());

  // The other half of Start()'s documented precondition: with autostart off,
  // kPrepared is where PrepareSync() leaves you, and Start() is the caller's
  // next legal move. No guard needed -- and none possible to write that is
  // correct in both configurations.
  EXPECT_EQ(impl.state(), PlayerState::kPrepared);
  impl.Start();
  EXPECT_EQ(impl.state(), PlayerState::kStarted);
  impl.StopSync(base::Milliseconds(500));
}

}  // namespace

}  // namespace avbase

#endif  // !AVBASE_ENABLE_FFMPEG