// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The two media-layer entries from the docs/04 §8 `BM_*` list whose budgets are
// the tightest, and the only two that can be timed without a decoder: both
// depend on media/base and media/filters alone, so AVBASE_ENABLE_FFMPEG=OFF
// still configures, builds and runs this target.
//
//   BM_GetMasterClock    budget < 200 ns  (seqlock read, 2-4 calls per frame)
//   BM_DecideNextFrame   budget < 1 us    (pure decision core, once per frame)
//
// Both budgets exist because of risk R3 in docs/08: this tree is a layered
// rewrite of ijkplayer, and every layer (virtual TimeSource, scoped_refptr,
// seqlock) can quietly start charging for itself. Only a timed number notices.

#include "bench.h"

#include "base/time/simple_test_tick_clock.h"
#include "base/time/time.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/legacy/video_frame_compositor.h"

namespace {

using avbase::base::SimpleTestTickClock;
using avbase::base::TimeDelta;
using avbase::base::TimeTicks;
using avbase::media::AvSyncController;
using avbase::media::VideoFrameCompositor;

// Same fixture the unit tests use (tests/unit/media_filters/
// video_frame_compositor_unittest.cc): a 30 fps stream whose next frame is due
// exactly now and is perfectly in sync with an audio master clock. Deliberately
// the *happy* path -- it is the one every played frame takes, and the one the
// < 1 us budget is written against.
//
// Coverage limit, stated rather than hidden: this measures the kPresent branch
// only. The kHold and kDrop branches take different code paths inside the same
// function and are NOT timed here; a regression confined to
// ShouldDropForLateness() would not show up in this number.
constexpr TimeDelta kFps30 = avbase::base::Microseconds(33333);
constexpr TimeTicks kNow = TimeTicks::FromMicroseconds(1000000);
constexpr TimeDelta kTenSeconds = avbase::base::Seconds(10);

VideoFrameCompositor::FrameSyncInput MakePresentInput() {
  VideoFrameCompositor::FrameSyncInput in;
  in.fps_duration = kFps30;
  in.next_duration = kFps30;
  in.next_timestamp = kTenSeconds;
  in.last_timestamp = kTenSeconds - kFps30;
  in.next_serial = 1;
  in.last_serial = 1;
  in.queue_serial = 1;
  in.master_clock = kTenSeconds;
  in.master_serial = 1;
  in.master_clock_valid = true;
  in.master_is_video = false;
  in.now = kNow;
  in.frame_timer = kNow - kFps30;
  in.last_present_wall_time = kNow - kFps30;
  in.displayed_duration = kFps30;
  in.playback_rate = 1.0;
  in.has_candidate = true;
  return in;
}

AVBASE_BENCH(BM_DecideNextFrame) {
  const VideoFrameCompositor::FrameSyncInput in = MakePresentInput();
  const VideoFrameCompositor::Thresholds th{};
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    const VideoFrameCompositor::FrameSyncOutput out =
        VideoFrameCompositor::DecideNextFrame(in, th);
    sink += static_cast<uint64_t>(out.decision);
  }
  avbase::bench::Sink(sink);
}

AVBASE_BENCH(BM_GetMasterClock) {
  // A frozen wall clock on purpose: GetMasterClock() extrapolates media time
  // from the instant the clock was anchored, and letting real time advance
  // would add a varying operand to a 200 ns measurement. The seqlock read path
  // is what is being timed, not the arithmetic on a moving clock.
  SimpleTestTickClock clock;
  AvSyncController controller(AvSyncController::MasterType::kAudio, &clock,
                              AvSyncController::Thresholds{});
  controller.SetStreamAvailability(/*has_audio=*/true, /*has_video=*/true);
  controller.OnAudioFramesConsumed(1024, avbase::base::Seconds(5), 1);

  int64_t sink = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    sink += controller.GetMasterClock().InMicroseconds();
  }
  avbase::bench::Sink(static_cast<uint64_t>(sink));
}

}  // namespace
