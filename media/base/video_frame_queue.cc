// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/video_frame_queue.h"

#include <utility>

#include "base/check.h"
#include "base/logging.h"

namespace ijkpp::media {

const char* GetVideoFrameQueuePopStatusName(VideoFrameQueue::PopStatus status) {
  using S = VideoFrameQueue::PopStatus;
  switch (status) {
    case S::kOk:          return "ok";
    case S::kEmpty:       return "empty";
    case S::kFlushed:     return "flushed";
    case S::kAborted:     return "aborted";
    case S::kEndOfStream: return "end-of-stream";
  }
  return "invalid";
}

// ---------------------------------------------------------------------------
// SlotGuard
// ---------------------------------------------------------------------------

VideoFrameQueue::SlotGuard::SlotGuard() : queue_(nullptr), slot_(0) {}

VideoFrameQueue::SlotGuard::SlotGuard(VideoFrameQueue* queue, size_t slot)
    : queue_(queue), slot_(slot) {}

VideoFrameQueue::SlotGuard::SlotGuard(SlotGuard&& other) noexcept
    : queue_(other.queue_), slot_(other.slot_) {
  other.queue_ = nullptr;
  other.slot_ = 0;
}

VideoFrameQueue::SlotGuard& VideoFrameQueue::SlotGuard::operator=(
    SlotGuard&& other) noexcept {
  if (this != &other) {
    Release();
    queue_ = other.queue_;
    slot_ = other.slot_;
    other.queue_ = nullptr;
    other.slot_ = 0;
  }
  return *this;
}

VideoFrameQueue::SlotGuard::~SlotGuard() { Release(); }

void VideoFrameQueue::SlotGuard::Release() {
  if (queue_) {
    // Returning an uncommitted slot is the whole point of the guard: it is what
    // makes the ijkplayer slot-leak deadlock structurally impossible.
    queue_->ReturnSlot(slot_, /*committed=*/false);
    queue_ = nullptr;
  }
}

void VideoFrameQueue::SlotGuard::Commit(
    base::scoped_refptr<VideoFrame> frame) {
  CHECK(queue_) << "SlotGuard::Commit() on an empty guard";
  CHECK(frame) << "SlotGuard::Commit() with a null frame";
  VideoFrameQueue* queue = queue_;
  const size_t slot = slot_;
  queue_ = nullptr;   // Transfer ownership of the slot to the queue.
  {
    base::AutoLock scoped(queue->lock_);
    CHECK_LT(slot, queue->slots_.size());
    CHECK(queue->slots_[slot].state == SlotState::kReserved);
    queue->slots_[slot].frame = std::move(frame);
    queue->slots_[slot].state = SlotState::kFilled;
    queue->committed_.fetch_add(1, std::memory_order_relaxed);
  }
  queue->frame_available_.Signal();
}

void VideoFrameQueue::SlotGuard::Abandon() { Release(); }

// ---------------------------------------------------------------------------
// VideoFrameQueue
// ---------------------------------------------------------------------------

VideoFrameQueue::VideoFrameQueue(std::string name, int capacity)
    : name_(std::move(name)),
      capacity_(capacity),
      slot_available_(&lock_),
      frame_available_(&lock_) {
  CHECK_GT(capacity_, 0);
  slots_.resize(static_cast<size_t>(capacity_));
}

VideoFrameQueue::~VideoFrameQueue() { Abort(); }

size_t VideoFrameQueue::FindFreeSlotLocked() const {
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (slots_[i].state == SlotState::kFree) {
      return i;
    }
  }
  return slots_.size();   // None free.
}

bool VideoFrameQueue::IsFullLocked() const {
  return FindFreeSlotLocked() == slots_.size();
}

VideoFrameQueue::SlotGuard VideoFrameQueue::Reserve() {
  base::AutoLock scoped(lock_);
  for (;;) {
    if (abort_flag_.IsSet() || closed_) {
      return SlotGuard();
    }
    const size_t index = FindFreeSlotLocked();
    if (index < slots_.size()) {
      slots_[index].state = SlotState::kReserved;
      return SlotGuard(this, index);
    }
    reserve_waits_.fetch_add(1, std::memory_order_relaxed);
    slot_available_.Wait();
  }
}

bool VideoFrameQueue::TryReserve(SlotGuard* out) {
  DCHECK(out);
  base::AutoLock scoped(lock_);
  if (abort_flag_.IsSet() || closed_ || IsFullLocked()) {
    return false;
  }
  const size_t index = FindFreeSlotLocked();
  slots_[index].state = SlotState::kReserved;
  *out = SlotGuard(this, index);
  return true;
}

void VideoFrameQueue::ReturnSlot(size_t index, bool committed) {
  {
    base::AutoLock scoped(lock_);
    if (index >= slots_.size()) {
      return;
    }
    if (!committed) {
      slots_[index].frame = nullptr;
      returned_uncommitted_.fetch_add(1, std::memory_order_relaxed);
    }
    slots_[index].state = SlotState::kFree;
  }
  slot_available_.Signal();
}

VideoFrameQueue::PopStatus VideoFrameQueue::Pop(
    base::scoped_refptr<VideoFrame>* out) {
  DCHECK(out);
  base::AutoLock scoped(lock_);
  for (;;) {
    if (abort_flag_.IsSet()) {
      return PopStatus::kAborted;
    }
    for (auto& slot : slots_) {
      if (slot.state == SlotState::kFilled) {
        *out = std::move(slot.frame);
        slot.state = SlotState::kFree;
        popped_.fetch_add(1, std::memory_order_relaxed);
        // A freed slot is exactly what a blocked producer is waiting for.
        slot_available_.Signal();
        return PopStatus::kOk;
      }
    }
    if (eos_ || closed_) {
      return eos_ ? PopStatus::kEndOfStream : PopStatus::kEmpty;
    }
    frame_available_.Wait();
  }
}

size_t VideoFrameQueue::Peek(
    std::vector<base::scoped_refptr<VideoFrame>>* out, size_t max) const {
  DCHECK(out);
  base::AutoLock scoped(lock_);
  for (const auto& slot : slots_) {
    if (out->size() >= max) {
      break;
    }
    if (slot.state == SlotState::kFilled) {
      out->push_back(slot.frame);
    }
  }
  return out->size();
}

void VideoFrameQueue::Flush() {
  size_t dropped = 0;
  {
    base::AutoLock scoped(lock_);
    for (auto& slot : slots_) {
      if (slot.state == SlotState::kFilled) {
        slot.frame = nullptr;
        ++dropped;
      }
      // Reserved slots are left alone: their guards still own them and will
      // return them on destruction. Marking them free here would let a second
      // producer reserve the same slot.
      if (slot.state == SlotState::kFilled) {
        slot.state = SlotState::kFree;
      }
    }
    eos_ = false;
    serial_.Add(1);
    dropped_by_flush_.fetch_add(dropped, std::memory_order_relaxed);
    slot_available_.Broadcast();
    frame_available_.Broadcast();
  }
  DVLOG(1) << name_ << ": flushed " << dropped << " frames, serial now "
           << serial_.Get();
}

void VideoFrameQueue::Abort() {
  abort_flag_.Set();
  base::AutoLock scoped(lock_);
  closed_ = true;
  slot_available_.Broadcast();
  frame_available_.Broadcast();
}

void VideoFrameQueue::MarkEndOfStream() {
  base::AutoLock scoped(lock_);
  eos_ = true;
  frame_available_.Broadcast();
}

size_t VideoFrameQueue::size() const {
  base::AutoLock scoped(lock_);
  size_t n = 0;
  for (const auto& slot : slots_) {
    if (slot.state == SlotState::kFilled) ++n;
  }
  return n;
}

size_t VideoFrameQueue::reserved_count() const {
  base::AutoLock scoped(lock_);
  size_t n = 0;
  for (const auto& slot : slots_) {
    if (slot.state == SlotState::kReserved) ++n;
  }
  return n;
}

bool VideoFrameQueue::end_of_stream() const {
  base::AutoLock scoped(lock_);
  return eos_;
}

VideoFrameQueue::Stats VideoFrameQueue::GetStats() const {
  base::AutoLock scoped(lock_);
  Stats stats;
  for (const auto& slot : slots_) {
    switch (slot.state) {
      case SlotState::kFilled:   ++stats.filled;   break;
      case SlotState::kReserved: ++stats.reserved; break;
      case SlotState::kFree:     break;
    }
  }
  stats.capacity = capacity_;
  stats.serial = serial_.Get();
  stats.committed = committed_.load(std::memory_order_relaxed);
  stats.popped = popped_.load(std::memory_order_relaxed);
  stats.slots_returned_uncommitted =
      returned_uncommitted_.load(std::memory_order_relaxed);
  stats.dropped_by_flush = dropped_by_flush_.load(std::memory_order_relaxed);
  stats.reserve_waits = reserve_waits_.load(std::memory_order_relaxed);
  stats.end_of_stream = eos_;
  stats.aborted = abort_flag_.IsSet();
  return stats;
}

}  // namespace ijkpp::media
