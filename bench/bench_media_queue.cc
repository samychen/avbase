// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Three more entries from the docs/04 §8 `BM_*` list -- the ones whose budgets
// are about per-packet and per-frame bookkeeping rather than about sync:
//
//   BM_DecoderBufferQueue_PushPop   budget < 5 us   (one AutoLock + refptr
//   move) BM_DecoderBuffer_MoveConstruct  budget: 0 allocations
//   BM_VideoFrame_RefCountChurn     budget: 0 allocations
//
// WHY THESE THREE AND NOT THE END-TO-END ONES: the remaining entries in that
// list (BM_EndToEnd_1080p_NullSink, BM_FirstFrameLatency, BM_SeekLatency,
// BM_MemoryPerInstance) all need a real decoder, and this target builds with
// AVBASE_ENABLE_FFMPEG=OFF on purpose -- it is a micro-benchmark gate, and
// making it depend on FFmpeg would make it depend on the pinned-FFmpeg setup
// step. These three need media/base only.
//
// They are the ones risk R3 (docs/08) is actually about: a layered rewrite
// where scoped_refptr, a lock or a virtual call can start charging for itself
// on the path every single packet takes. Two of the three budgets are
// "allocation-free", which no correctness test can see and which a profiler
// only finds after someone notices a regression.

#include "bench.h"

#include <utility>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_buffer_queue.h"
#include "media/base/decoder_config.h"
#include "media/base/media_log.h"
#include "media/base/video_frame.h"

namespace {

using avbase::base::Microseconds;
using avbase::base::scoped_refptr;
using avbase::media::DecoderBuffer;
using avbase::media::DecoderBufferQueue;
using avbase::media::DemuxerStreamType;
using avbase::media::MediaLog;
using avbase::media::VideoFormat;
using avbase::media::VideoFrame;

// One compressed packet, ~ a 1080p I-frame's worth of bytes. Reused across
// iterations: the thing being timed is the queue's bookkeeping, not the copy.
scoped_refptr<DecoderBuffer> MakeBuffer() {
  static const std::vector<uint8_t> kPayload(4096, 0x5A);
  return DecoderBuffer::CopyFrom(kPayload.data(), kPayload.size(),
                                 DemuxerStreamType::kVideo, 0);
}

// A queue deep enough that Push never blocks (blocking would time a condition
// variable wait, not the queue) and non-blocking PopUpTo on the read side.
DecoderBufferQueue MakeQueue() {
  return DecoderBufferQueue("bench", /*max_buffers=*/4096,
                            /*max_bytes=*/0,
                            avbase::base::MakeRefCounted<MediaLog>());
}

AVBASE_BENCH(BM_DecoderBufferQueue_PushPop) {
  DecoderBufferQueue queue = MakeQueue();
  scoped_refptr<DecoderBuffer> buffer = MakeBuffer();
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    // TryPush + PopUpTo rather than Push + Pop: the latter pair blocks at
    // either end of an empty/full queue, and a blocking wait would dominate a
    // 5 us budget while telling us nothing about the queue itself.
    if (!queue.TryPush(buffer)) {
      break;
    }
    std::vector<scoped_refptr<DecoderBuffer>> out;
    out.reserve(1);
    if (queue.PopUpTo(1, &out) != DecoderBufferQueue::PopStatus::kOk) {
      break;
    }
    sink += out.size();
  }
  avbase::bench::Sink(sink);
  queue.Abort();
}

// The packet hand-off itself: one refptr move-construct + one move-assign,
// which is what a demuxer→decoder hop costs on top of the queue. Budget is
// "0 allocations" -- a refcount bump, not a copy.
AVBASE_BENCH(BM_DecoderBuffer_MoveConstruct) {
  scoped_refptr<DecoderBuffer> held = MakeBuffer();
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    scoped_refptr<DecoderBuffer> moved(std::move(held));
    sink += moved->data_size();
    // Give it back so the next iteration moves a live object: moving out of a
    // null refptr would time a branch, not a refcount.
    held = std::move(moved);
  }
  avbase::bench::Sink(sink);
}

// One frame, AddRef'd and released through a second handle. This is the cost a
// compositor pays to keep a frame alive across a present -- every frame, and
// the reason VideoFrame is refcounted rather than copied.
AVBASE_BENCH(BM_VideoFrame_RefCountChurn) {
  scoped_refptr<VideoFrame> frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, avbase::media::Size{1920, 1080},
      avbase::media::Size{1920, 1080}, avbase::media::Rational{30, 1},
      Microseconds(0), Microseconds(33333), 0);
  if (!frame) {
    return;  // No frame allocator compiled in; nothing to measure.
  }
  uint64_t sink = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    {
      scoped_refptr<VideoFrame> second = frame;
      sink += static_cast<uint64_t>(second->coded_size().width);
    }  // release
  }
  avbase::bench::Sink(sink);
}

}  // namespace
