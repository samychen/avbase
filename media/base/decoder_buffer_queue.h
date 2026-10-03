// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_BASE_DECODER_BUFFER_QUEUE_H_
#define AVBASE_MEDIA_BASE_DECODER_BUFFER_QUEUE_H_

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
#include "base/time/time.h"
#include "media/base/decoder_buffer.h"
#include "media/base/media_log.h"
#include "media/media_export.h"

namespace avbase::media {

// Bounded, serial-aware queue of compressed samples between a demuxer and a
// decoder.
//
// Replaces ijkplayer's packet_queue_* function family (nine C functions sharing
// a PacketQueue struct embedded in the 200-field VideoState). Three properties
// are load-bearing and are covered by unit tests:
//
//  1. SERIAL. Flush() bumps a generation counter and stamps every later buffer
//     with it. A consumer that sees buffer->serial() < queue.serial() must drop
//     the buffer without decoding it, otherwise pre-seek frames reach the
//     decoder after a seek. See docs/04 §4 rules R1-R3.
//  2. BACKPRESSURE. Push() blocks when either the count or the byte limit is
//     reached, which is what stops the demuxer from reading a whole file into
//     RAM. Blocking is interruptible through Abort().
//  3. BOUNDED SHUTDOWN. Abort() wakes every waiter immediately and makes all
//     further operations fail fast, so ~Player() cannot hang on a producer
//     blocked against a full queue (docs/04 §5.4, Δ15).
class AVBASE_MEDIA_EXPORT DecoderBufferQueue {
 public:
  enum class PopStatus {
    kOk,           // |out| holds a buffer.
    kEmpty,        // Queue is empty and closed for further input.
    kFlushed,      // A Flush() happened while waiting; retry.
    kAborted,      // Abort() was called; the consumer should exit.
    kEndOfStream,  // The EOS marker was reached.
  };

  struct Stats {
    size_t buffers{0};
    size_t bytes{0};
    int32_t serial{0};
    base::TimeDelta cached_duration;
    uint64_t pushed{0};
    uint64_t popped{0};
    uint64_t dropped_by_flush{0};
    uint64_t push_waits{0};
    uint64_t pop_waits{0};
    bool end_of_stream{false};
    bool aborted{false};
  };

  DecoderBufferQueue(std::string name, size_t max_buffers, size_t max_bytes,
                     base::scoped_refptr<MediaLog> media_log);
  DecoderBufferQueue(const DecoderBufferQueue&) = delete;
  DecoderBufferQueue& operator=(const DecoderBufferQueue&) = delete;
  ~DecoderBufferQueue();

  // Blocks while the queue is at either limit. Returns false if Abort() was
  // called, in which case |buffer| is dropped.
  bool Push(base::scoped_refptr<DecoderBuffer> buffer);
  // Never blocks. Returns false when full, aborted, or closed.
  bool TryPush(base::scoped_refptr<DecoderBuffer> buffer);

  // Text-stream variant: when the queue is at a limit, drop the OLDEST
  // buffer instead of failing. Subtitle packets arrive once and are small;
  // a text leg that selects late must still see the track, and a full text
  // queue must never wedge the demux loop (nobody else would drain it).
  // Returns false only on abort/closed, like TryPush.
  bool TryPushDropOldest(base::scoped_refptr<DecoderBuffer> buffer);

  // Blocks until a buffer is available. Returns the reason it stopped.
  PopStatus Pop(base::scoped_refptr<DecoderBuffer>* out);
  // Non-blocking; takes up to |max| buffers. Returns kEmpty when none are
  // ready.
  PopStatus PopUpTo(size_t max,
                    std::vector<base::scoped_refptr<DecoderBuffer>>* out);

  // Clears the queue, bumps the serial and wakes every waiter with kFlushed.
  void Flush();
  // Permanently shuts the queue down. Wakes every waiter with kAborted.
  void Abort();
  // Appends the EOS marker. Consumers see kEndOfStream once they reach it.
  void MarkEndOfStream();

  int32_t serial() const { return serial_.Get(); }
  size_t size() const;
  size_t bytes() const;
  bool empty() const;
  bool end_of_stream() const;
  bool aborted() const { return abort_flag_.IsSet(); }
  // Last timestamp minus first timestamp; the value BufferController uses to
  // decide whether the three-tier high water mark has been reached.
  base::TimeDelta buffered_duration() const;
  Stats GetStats() const;
  const std::string& name() const { return name_; }

 private:
  // The *Locked suffix is the contract: the caller already holds |lock_|.
  bool IsFullLocked() const EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void UpdateCachedDurationLocked() EXCLUSIVE_LOCKS_REQUIRED(lock_);

  const std::string name_;
  const size_t max_buffers_;
  const size_t max_bytes_;
  const base::scoped_refptr<MediaLog> media_log_;

  mutable base::Lock lock_;
  base::ConditionVariable not_full_;
  base::ConditionVariable not_empty_;
  std::deque<base::scoped_refptr<DecoderBuffer>> buffers_ GUARDED_BY(lock_);
  size_t bytes_ GUARDED_BY(lock_){0};
  bool closed_ GUARDED_BY(lock_){false};
  bool eos_ GUARDED_BY(lock_){false};
  base::TimeDelta cached_duration_ GUARDED_BY(lock_);

  base::AtomicSequenceNumber serial_;
  base::AtomicFlag abort_flag_;

  // Counters read from other sequences for diagnostics; relaxed is fine.
  std::atomic<uint64_t> pushed_{0};
  std::atomic<uint64_t> popped_{0};
  std::atomic<uint64_t> dropped_by_flush_{0};
  std::atomic<uint64_t> push_waits_{0};
  std::atomic<uint64_t> pop_waits_{0};
};

AVBASE_MEDIA_EXPORT const char*
GetPopStatusName(DecoderBufferQueue::PopStatus s);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_DECODER_BUFFER_QUEUE_H_
