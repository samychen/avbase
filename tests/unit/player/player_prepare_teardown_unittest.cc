// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Preparing a player and tearing it down again WITHOUT waiting for the
// prepare verdict -- the "open a URL, change your mind" host pattern.
//
// WHY THIS EXISTS. player_deps_wiring_unittest.cc drives exactly this shape
// (prepare, then let the helper's PlayerImpl die), and on its first run it
// caught two teardown races in the same path, in two different runs:
//
//   1. The demuxer's open completion was posted to the media sequence with a
//      raw pointer. Stop() joins the demux thread -- which DRAINS the open
//      task -- and the owner may destroy the demuxer the moment Stop()
//      returns, i.e. while the completion is still only queued. Result: a
//      use-after-free that the media-sequence DCHECK reports in a debug
//      build and a release build corrupts silently. Fixed by binding the
//      completion through FFmpegDemuxer's WeakPtr (docs/04 §7 R11).
//
//   2. PipelineImpl::Start() left the state at kNew until the media sequence
//      ran DoStart, and IsRunning() -- the "safe to destroy" predicate
//      StopSync polls -- does not count kNew. A teardown that overtook
//      DoStart was therefore told "safe" before anything had run: it stopped
//      the media thread, DoStop was dropped from its queue, and the pipeline
//      was destroyed mid-kStarting (~PipelineImpl's DCHECK names exactly
//      that). Fixed by making the kNew -> kStarting transition synchronous
//      in Start().
//
// WHAT THIS PINS. Neither race is asserted through a value: both were
// DCHECK failures (a FATAL, or a SEGV in a release build), so the pin is
// "the teardown completes cleanly, every time". Reverting either fix turns
// this red -- run it 5x to be sure, the loop below is 20 iterations for the
// same reason: the races are timing-dependent in the red direction (they
// need the main thread to overtake the media sequence, which thread-startup
// latency makes near-certain but not guaranteed), and a regression test
// that only fails 99% of the time is still a regression test.

#include "player/player_impl.h"

#include <memory>
#include <string>

#include "avbase/BuildConfig.h"
#include "gtest/gtest.h"
#include "player/public/player_config.h"

#if !AVBASE_ENABLE_FFMPEG
// Same reason as player_deps_wiring_unittest.cc: without a demuxer there is
// no prepare to tear down, so there is no teardown race to pin. Compiles to
// nothing rather than registering tests that would have to skip.
#else

namespace avbase {
namespace {

// A path no machine has. The demuxer fails on the media sequence; what is on
// trial is what the teardown does with a pipeline that never finished
// starting.
constexpr char kUnopenableUri[] = "/nonexistent/avbase-teardown-test.mp4";

}  // namespace

TEST(PlayerPrepareTeardownTest, DestroyingMidPrepareLeavesNothingBehind) {
  for (int i = 0; i < 20; ++i) {
    PlayerImpl impl(PlayerConfig(), nullptr);
    ASSERT_TRUE(impl.SetDataSource(kUnopenableUri).has_value());
    // The whole test: no observer, no event handler, no waiting for
    // kPrepared/kError. PrepareAsync returns as soon as the pipeline's start
    // is posted, and ~PlayerImpl runs against a pipeline that is still
    // starting. StopSync must not call that safe, and the demuxer's open
    // completion must not outlive the demuxer.
    ASSERT_TRUE(impl.PrepareAsync().has_value());
  }
}

}  // namespace avbase

#endif  // !AVBASE_ENABLE_FFMPEG
