// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_BASE_LATEST_FRAME_QUEUE_H_
#define AVBASE_MEDIA_BASE_LATEST_FRAME_QUEUE_H_

#include <stdint.h>

#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "media/base/video_frame.h"
#include "media/media_export.h"

namespace avbase::media {

// A one-slot mailbox between a producer and a consumer that run at different
// rates.
//
// This is NOT a queue, and the difference is the whole design. A queue makes
// every frame the consumer's problem; a mailbox makes the newest one the only
// one, which is what a display wants -- a picture that is one refresh late is
// worth nothing, and a picture that is ten refreshes late is worth less. So
// Publish() replaces whatever is waiting instead of lining up behind it, and
// Consume() takes the newest and leaves the slot empty.
//
// WHY PUBLISH() RETURNS A FRAME. The producer is the only observer of a
// superseded frame: the consumer never sees it, so nothing downstream can
// count it. Handing it back is therefore the only way "a frame was decoded and
// never displayed" is knowable at all -- the alternative is a counter that
// silently omits the frames that were dropped, which is the one kind of
// statistic that makes a pipeline look healthier than it is. What the caller
// does with it (a pool release, a drop counter, nothing) is the caller's
// business.
//
// WHY IT IS ONE SLOT AND NOT N. A depth of N is a latency budget: it says "a
// frame may be up to N deep before it is thrown away", which is a decision
// about how stale a picture may be. One slot says the only two states are
// "the newest frame, not yet displayed" and "nothing new" -- no number to
// tune, and no way for a producer that runs ahead to accumulate a backlog of
// frames whose pixels were already replaced by a newer decode.
//
// THREADING: Publish() and Consume() may be called from any thread and from
// different threads. Neither blocks on the other for more than the swap.
class AVBASE_MEDIA_EXPORT LatestFrameQueue {
 public:
  LatestFrameQueue();
  LatestFrameQueue(const LatestFrameQueue&) = delete;
  LatestFrameQueue& operator=(const LatestFrameQueue&) = delete;
  ~LatestFrameQueue();

  // Producer side. |frame| must not be null. Returns the frame that was
  // waiting -- the one this call superseded -- or null when the slot was
  // empty, i.e. when the consumer had already taken the previous one.
  base::scoped_refptr<VideoFrame>
  Publish(base::scoped_refptr<VideoFrame> frame);

  // Consumer side. Destructive: takes the newest frame and empties the slot.
  // Null when nothing has been published since the last Consume() or Clear().
  //
  // Not a wait. A display that is asked for a frame and has none gets null and
  // decides for itself what that means (keep the last picture, draw black,
  // skip the present) -- blocking here would put the producer's rate in charge
  // of the consumer's deadline, which is the inversion this class exists to
  // prevent.
  base::scoped_refptr<VideoFrame> Consume();

  // Drops the frame that is waiting and returns it; null when nothing was
  // waiting. What a flush or a seek wants: a picture decoded before the jump
  // must not appear after it, and "the consumer has not asked yet" is not a
  // reason to show it.
  //
  // Distinct from being superseded in the counts below because the two are
  // different diagnoses: superseded means the producer outran the consumer,
  // cleared means somebody ordered the queue emptied. A caller reading only
  // the total would not be able to tell a slow consumer from a seek.
  base::scoped_refptr<VideoFrame> Clear();

  bool has_pending() const;

  // ---- Counts. Every one of them is observable in exactly one place --------
  //
  //   published == consumed + superseded + cleared + (pending ? 1 : 0)
  //
  // Every frame published is accounted for on the right, and the four terms
  // are kept apart because "which of the three ways of not being displayed
  // happened" is the question a stalled or juddering pipeline is actually
  // asking. Collapsing them into a single "dropped" would answer "some frames
  // did not make it", which nobody was unsure about.
  uint64_t published_frames() const;
  uint64_t consumed_frames() const;
  uint64_t superseded_frames() const;
  uint64_t cleared_frames() const;

 private:
  mutable base::Lock lock_;
  base::scoped_refptr<VideoFrame> pending_ GUARDED_BY(lock_);
  uint64_t published_ GUARDED_BY(lock_){0};
  uint64_t consumed_ GUARDED_BY(lock_){0};
  uint64_t superseded_ GUARDED_BY(lock_){0};
  uint64_t cleared_ GUARDED_BY(lock_){0};
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_LATEST_FRAME_QUEUE_H_
