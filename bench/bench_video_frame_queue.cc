// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The docs/04 §8 entry `VideoFrameQueue::Reserve/Commit` (budget < 5 us). It
// sat in that document's "未落地" list with the note "树里没有同名类型",
// which described an older FrameQueue under media/filters/legacy. The
// two-phase queue the budget is actually about now exists as
// media/base/video_frame_queue.h -- Reserve() hands out a SlotGuard,
// Commit() publishes, and the guard returns the slot on every other path --
// and it is what the decoder pushes into. The note was stale, not the
// budget.
//
// WHY THIS ONE IS ON THE LIST: the write path runs once per decoded frame
// between the decoder and the renderer, and its cost is one lock plus slot
// bookkeeping plus two refptr moves. A regression there (a copy instead of a
// move, a second lock, an allocation per slot) is invisible to every
// correctness test and shows up only as playback CPU, which is risk R3 in
// docs/08 -- the reason this benchmark family exists.

#include "bench.h"

#include <cstdint>
#include <utility>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/video_frame.h"
#include "media/base/video_frame_queue.h"

namespace {

using avbase::base::Microseconds;
using avbase::base::scoped_refptr;
using avbase::media::Rational;
using avbase::media::Size;
using avbase::media::VideoFormat;
using avbase::media::VideoFrame;
using avbase::media::VideoFrameQueue;

// One 1080p frame, built once outside the timed region. The queue moves
// refptrs and never touches pixels, so frame content is irrelevant to what is
// being measured; what matters is that the frame is alive for the whole run.
scoped_refptr<VideoFrame> MakeFrame() {
  return VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{1920, 1080}, Size{1920, 1080}, Rational{30, 1},
      Microseconds(0), Microseconds(33333), 0);
}

AVBASE_BENCH(BM_VideoFrameQueue_ReserveCommit) {
  scoped_refptr<VideoFrame> frame = MakeFrame();
  if (!frame) {
    return;  // No frame allocator compiled in; nothing to measure.
  }
  // Capacity 4 with one frame resident: TryReserve always finds a free slot
  // and Pop always finds a frame, so neither end ever waits on the condition
  // variable. A blocking wait would dominate a 5 us budget while measuring
  // the scheduler, not the queue -- the same reason
  // BM_DecoderBufferQueue_PushPop times TryPush + PopUpTo instead of the
  // blocking pair.
  //
  // The round trip is timed as a whole (reserve + commit + pop) rather than
  // the write half alone: timing only writes would fill the queue and then
  // time a wait, and the read half is what makes the steady state possible.
  VideoFrameQueue queue("bench", /*capacity=*/4);
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    VideoFrameQueue::SlotGuard guard;
    if (!queue.TryReserve(&guard)) {
      break;  // Only false after Abort(); cannot happen in this loop.
    }
    guard.Commit(frame);
    scoped_refptr<VideoFrame> out;
    if (queue.Pop(&out) != VideoFrameQueue::PopStatus::kOk) {
      break;  // A frame was just committed; this cannot happen either.
    }
    sink += static_cast<uint64_t>(out->coded_size().width);
  }
  avbase::bench::Sink(sink);
  queue.Abort();
}

}  // namespace
