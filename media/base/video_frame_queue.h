// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_BASE_VIDEO_FRAME_QUEUE_H_
#define AVBASE_MEDIA_BASE_VIDEO_FRAME_QUEUE_H_

#include <stdint.h>

#include <atomic>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/synchronization/atomic_flag.h"
#include "base/synchronization/atomic_sequence_number.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "media/base/video_frame.h"
#include "media/media_export.h"

namespace avbase::media {

// Bounded, serial-aware queue of decoded frames between a decoder and the
// renderer, with a two-phase (reserve / commit) write protocol.
//
// WHY TWO PHASES. ijkplayer's frame_queue_peek_writable() hands the caller a
// raw Frame* that must be filled in and then passed to frame_queue_push(). Any
// early return between the two — a decode error, an abort check, a resolution
// change — leaks that slot permanently. The queue has a fixed number of slots,
// so after enough leaks the decoder blocks forever waiting for a writable slot
// and playback hangs with no error and no log. That is a real ijkplayer bug
// class, and it is unfixable by discipline alone because the leak sites are
// scattered across three thread functions.
//
// Here Reserve() returns a SlotGuard. Committing publishes the frame; letting
// the guard go out of scope returns the slot. There is no code path that can
// forget.
class AVBASE_MEDIA_EXPORT VideoFrameQueue {
 public:
  enum class PopStatus { kOk, kEmpty, kFlushed, kAborted, kEndOfStream };

  class AVBASE_MEDIA_EXPORT SlotGuard {
   public:
    SlotGuard();
    SlotGuard(SlotGuard&& other) noexcept;
    SlotGuard& operator=(SlotGuard&& other) noexcept;
    SlotGuard(const SlotGuard&) = delete;
    SlotGuard& operator=(const SlotGuard&) = delete;
    // Returns the slot if Commit() was never called.
    ~SlotGuard();

    // Publishes |frame|. The guard becomes empty; its destructor then does
    // nothing. Must be called at most once.
    void Commit(base::scoped_refptr<VideoFrame> frame);
    // Explicitly gives the slot back without publishing.
    void Abandon();
    explicit operator bool() const { return queue_ != nullptr; }

   private:
    friend class VideoFrameQueue;
    SlotGuard(VideoFrameQueue* queue, size_t slot);
    void Release();

    VideoFrameQueue* queue_;
    size_t slot_;
  };

  struct Stats {
    size_t filled{0};
    size_t reserved{0};
    int capacity{0};
    int32_t serial{0};
    uint64_t committed{0};
    uint64_t popped{0};
    uint64_t slots_returned_uncommitted{0};   // Leaks that RAII prevented.
    uint64_t dropped_by_flush{0};
    uint64_t reserve_waits{0};
    bool end_of_stream{false};
    bool aborted{false};
  };

  VideoFrameQueue(std::string name, int capacity);
  VideoFrameQueue(const VideoFrameQueue&) = delete;
  VideoFrameQueue& operator=(const VideoFrameQueue&) = delete;
  ~VideoFrameQueue();

  // Blocks until a slot is free. Returns an empty guard if Abort() was called.
  SlotGuard Reserve();
  bool TryReserve(SlotGuard* out);

  // Blocks until a frame is available.
  PopStatus Pop(base::scoped_refptr<VideoFrame>* out);
  // Non-consuming look-ahead, oldest first. The compositor uses this to derive
  // the inter-frame duration without removing anything.
  size_t Peek(std::vector<base::scoped_refptr<VideoFrame>>* out,
              size_t max) const;

  void Flush();
  void Abort();
  void MarkEndOfStream();

  size_t size() const;
  size_t reserved_count() const;
  int capacity() const { return capacity_; }
  int32_t serial() const { return serial_.Get(); }
  bool end_of_stream() const;
  bool aborted() const { return abort_flag_.IsSet(); }
  Stats GetStats() const;
  const std::string& name() const { return name_; }

 private:
  friend class SlotGuard;

  enum class SlotState { kFree, kReserved, kFilled };
  struct Slot {
    SlotState state{SlotState::kFree};
    base::scoped_refptr<VideoFrame> frame;
  };

  // Called by SlotGuard. Returns the slot to the free pool and wakes a waiter.
  void ReturnSlot(size_t index, bool committed);
  // The *Locked suffix is the contract: the caller already holds |lock_|.
  size_t FindFreeSlotLocked() const EXCLUSIVE_LOCKS_REQUIRED(lock_);
  bool IsFullLocked() const EXCLUSIVE_LOCKS_REQUIRED(lock_);

  const std::string name_;
  const int capacity_;

  mutable base::Lock lock_;
  base::ConditionVariable slot_available_;
  base::ConditionVariable frame_available_;
  std::deque<Slot> slots_ GUARDED_BY(lock_);
  bool eos_ GUARDED_BY(lock_){false};
  bool closed_ GUARDED_BY(lock_){false};

  base::AtomicSequenceNumber serial_;
  base::AtomicFlag abort_flag_;

  std::atomic<uint64_t> committed_{0};
  std::atomic<uint64_t> popped_{0};
  std::atomic<uint64_t> returned_uncommitted_{0};
  std::atomic<uint64_t> dropped_by_flush_{0};
  std::atomic<uint64_t> reserve_waits_{0};
};

AVBASE_MEDIA_EXPORT const char* GetVideoFrameQueuePopStatusName(
    VideoFrameQueue::PopStatus status);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_VIDEO_FRAME_QUEUE_H_
