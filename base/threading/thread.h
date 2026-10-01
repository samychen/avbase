// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `base/threading/thread.h` (BSD-3-Clause), restricted to
// what avbase needs: one TaskQueue-driven sequence per thread.

#ifndef AVBASE_BASE_THREADING_THREAD_H_
#define AVBASE_BASE_THREADING_THREAD_H_

#include <atomic>
#include <string>
#include <thread>

#include "base/base_export.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/task_queue.h"
#include "base/threading/platform_thread.h"

namespace avbase::base {

// A dedicated thread running a TaskQueue.
//
// avbase creates exactly six of these per Player (docs/04 §2): avbase-media,
// avbase-demux, avbase-video, avbase-audio, avbase-text and avbase-event.
// Each owns a set of objects that only it may touch, which is what
// SEQUENCE_CHECKER verifies at runtime.
//
// Stop() is bounded and idempotent: it asks the queue to quit and joins.
// Because Quit() also makes PostTask() return false, no task can be enqueued
// against a thread that is going away — the guarantee that makes ~Player()
// unable to hang on a producer blocked in a full queue (docs/04 §5.4, Δ15).
class AVBASE_BASE_EXPORT Thread {
 public:
  struct Options {
    std::string name;
    ThreadPriority priority{ThreadPriority::kBestEffort};
    // Size hint only; the queue itself grows as needed.
    size_t stack_size{0};
  };

  explicit Thread(std::string name);
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;
  // Calls Stop() if still running.
  ~Thread();

  // Starts the thread. Returns false if it is already running or the OS
  // refused.
  bool Start();
  bool StartWithOptions(const Options& options);

  // Requests quit and joins. Bounded by the OS join; the queue drains its
  // already-accepted tasks first unless |flush| is false.
  void Stop();

  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  // Valid as soon as the Thread is constructed, even before Start(): tasks
  // posted early are queued and run once the loop begins.
  scoped_refptr<SequencedTaskRunner> task_runner() const { return queue_; }
  TaskQueue* queue() const { return queue_.get(); }

  const std::string& name() const { return options_.name; }
  std::thread::id GetThreadId() const { return thread_id_.load(); }

 private:
  void ThreadMain();

  Options options_;
  scoped_refptr<TaskQueue> queue_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<std::thread::id> thread_id_{};
  WaitableEvent started_;
};

}  // namespace base

#endif  // AVBASE_BASE_THREADING_THREAD_H_
