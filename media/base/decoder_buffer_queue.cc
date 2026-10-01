// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/decoder_buffer_queue.h"

#include <utility>

#include "base/check.h"
#include "base/logging.h"

namespace avbase::media {

const char* GetPopStatusName(DecoderBufferQueue::PopStatus s) {
  using S = DecoderBufferQueue::PopStatus;
  switch (s) {
    case S::kOk:          return "ok";
    case S::kEmpty:       return "empty";
    case S::kFlushed:     return "flushed";
    case S::kAborted:     return "aborted";
    case S::kEndOfStream: return "end-of-stream";
  }
  return "invalid";
}

DecoderBufferQueue::DecoderBufferQueue(std::string name, size_t max_buffers,
                                       size_t max_bytes,
                                       base::scoped_refptr<MediaLog> media_log)
    : name_(std::move(name)),
      max_buffers_(max_buffers),
      max_bytes_(max_bytes),
      media_log_(std::move(media_log)),
      not_full_(&lock_),
      not_empty_(&lock_) {
  CHECK_GT(max_buffers_, 0u);
}

DecoderBufferQueue::~DecoderBufferQueue() {
  // Anything still queued is released here. Buffers are refcounted, so a
  // consumer holding one is unaffected.
  Abort();
}

bool DecoderBufferQueue::IsFullLocked() const {
  return buffers_.size() >= max_buffers_ ||
         (max_bytes_ > 0 && bytes_ >= max_bytes_);
}

void DecoderBufferQueue::UpdateCachedDurationLocked() {
  if (buffers_.size() < 2) {
    cached_duration_ = base::TimeDelta();
    return;
  }
  const base::TimeDelta first = buffers_.front()->BestEffortTimestamp();
  const base::TimeDelta last = buffers_.back()->BestEffortTimestamp();
  if (IsNoTimestamp(first) || IsNoTimestamp(last)) {
    cached_duration_ = base::TimeDelta();
    return;
  }
  cached_duration_ = last - first;
  if (cached_duration_.is_negative()) {
    cached_duration_ = base::TimeDelta();
  }
}

bool DecoderBufferQueue::Push(base::scoped_refptr<DecoderBuffer> buffer) {
  if (!buffer || abort_flag_.IsSet()) {
    return false;
  }
  base::AutoLock scoped(lock_);
  while (IsFullLocked() && !closed_ && !abort_flag_.IsSet()) {
    push_waits_.fetch_add(1, std::memory_order_relaxed);
    not_full_.Wait();
  }
  if (abort_flag_.IsSet() || closed_) {
    return false;
  }
  buffer->set_serial(serial_.Get());
  bytes_ += buffer->data_size();
  buffers_.push_back(std::move(buffer));
  pushed_.fetch_add(1, std::memory_order_relaxed);
  UpdateCachedDurationLocked();
  not_empty_.Signal();
  return true;
}

bool DecoderBufferQueue::TryPush(base::scoped_refptr<DecoderBuffer> buffer) {
  if (!buffer || abort_flag_.IsSet()) {
    return false;
  }
  base::AutoLock scoped(lock_);
  if (IsFullLocked() || closed_ || abort_flag_.IsSet()) {
    return false;
  }
  buffer->set_serial(serial_.Get());
  bytes_ += buffer->data_size();
  buffers_.push_back(std::move(buffer));
  pushed_.fetch_add(1, std::memory_order_relaxed);
  UpdateCachedDurationLocked();
  not_empty_.Signal();
  return true;
}

DecoderBufferQueue::PopStatus DecoderBufferQueue::Pop(
    base::scoped_refptr<DecoderBuffer>* out) {
  DCHECK(out);
  base::AutoLock scoped(lock_);
  for (;;) {
    if (abort_flag_.IsSet()) {
      return PopStatus::kAborted;
    }
    if (!buffers_.empty()) {
      base::scoped_refptr<DecoderBuffer> buffer = std::move(buffers_.front());
      buffers_.pop_front();
      bytes_ -= buffer->data_size();
      UpdateCachedDurationLocked();
      not_full_.Signal();
      if (buffer->IsEndOfStream()) {
        *out = std::move(buffer);
        return PopStatus::kEndOfStream;
      }
      popped_.fetch_add(1, std::memory_order_relaxed);
      *out = std::move(buffer);
      return PopStatus::kOk;
    }
    // eos_ must be checked before closed_: "the stream ended" and "the queue
    // was shut down" are different facts, and a consumer that cannot tell them
    // apart will keep polling a finished stream forever.
    if (eos_) {
      return PopStatus::kEndOfStream;
    }
    if (closed_) {
      return PopStatus::kEmpty;
    }
    pop_waits_.fetch_add(1, std::memory_order_relaxed);
    not_empty_.Wait();
  }
}

DecoderBufferQueue::PopStatus DecoderBufferQueue::PopUpTo(
    size_t max, std::vector<base::scoped_refptr<DecoderBuffer>>* out) {
  DCHECK(out);
  base::AutoLock scoped(lock_);
  if (abort_flag_.IsSet()) {
    return PopStatus::kAborted;
  }
  if (buffers_.empty()) {
    return eos_ ? PopStatus::kEndOfStream : PopStatus::kEmpty;
  }
  bool saw_eos = false;
  while (!buffers_.empty() && out->size() < max) {
    base::scoped_refptr<DecoderBuffer> buffer = std::move(buffers_.front());
    buffers_.pop_front();
    bytes_ -= buffer->data_size();
    if (buffer->IsEndOfStream()) {
      saw_eos = true;
      out->push_back(std::move(buffer));
      break;
    }
    popped_.fetch_add(1, std::memory_order_relaxed);
    out->push_back(std::move(buffer));
  }
  UpdateCachedDurationLocked();
  not_full_.Signal();
  return saw_eos ? PopStatus::kEndOfStream : PopStatus::kOk;
}

void DecoderBufferQueue::Flush() {
  size_t dropped = 0;
  {
    base::AutoLock scoped(lock_);
    dropped = buffers_.size();
    buffers_.clear();
    bytes_ = 0;
    cached_duration_ = base::TimeDelta();
    eos_ = false;
    // The serial bump is the whole point: buffers already handed to a consumer
    // carry the old value and are dropped there (rule R3).
    serial_.Add(1);
    dropped_by_flush_.fetch_add(dropped, std::memory_order_relaxed);
    not_full_.Broadcast();
    not_empty_.Broadcast();
  }
  if (media_log_ && dropped > 0) {
    media_log_->AddEvent(MediaLogEvent::Level::kInfo,
                         MediaLogEvent::Type::kSeekStarted,
                         {{"queue", name_},
                          {"dropped_buffers", std::to_string(dropped)},
                          {"new_serial", std::to_string(serial_.Get())}},
                         "queue flushed");
  }
}

void DecoderBufferQueue::Abort() {
  abort_flag_.Set();
  base::AutoLock scoped(lock_);
  closed_ = true;
  not_full_.Broadcast();
  not_empty_.Broadcast();
}

void DecoderBufferQueue::MarkEndOfStream() {
  base::AutoLock scoped(lock_);
  eos_ = true;
  buffers_.push_back(DecoderBuffer::CreateEOSBuffer());
  not_empty_.Broadcast();
}

size_t DecoderBufferQueue::size() const {
  base::AutoLock scoped(lock_);
  return buffers_.size();
}

size_t DecoderBufferQueue::bytes() const {
  base::AutoLock scoped(lock_);
  return bytes_;
}

bool DecoderBufferQueue::empty() const {
  base::AutoLock scoped(lock_);
  return buffers_.empty();
}

bool DecoderBufferQueue::end_of_stream() const {
  base::AutoLock scoped(lock_);
  return eos_;
}

base::TimeDelta DecoderBufferQueue::buffered_duration() const {
  base::AutoLock scoped(lock_);
  return cached_duration_;
}

DecoderBufferQueue::Stats DecoderBufferQueue::GetStats() const {
  base::AutoLock scoped(lock_);
  Stats stats;
  stats.buffers = buffers_.size();
  stats.bytes = bytes_;
  stats.serial = serial_.Get();
  stats.cached_duration = cached_duration_;
  stats.pushed = pushed_.load(std::memory_order_relaxed);
  stats.popped = popped_.load(std::memory_order_relaxed);
  stats.dropped_by_flush = dropped_by_flush_.load(std::memory_order_relaxed);
  stats.push_waits = push_waits_.load(std::memory_order_relaxed);
  stats.pop_waits = pop_waits_.load(std::memory_order_relaxed);
  stats.end_of_stream = eos_;
  stats.aborted = abort_flag_.IsSet();
  return stats;
}

}  // namespace avbase::media
