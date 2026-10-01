// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/threading/thread.h"

#include <utility>

#include "base/check.h"
#include "base/logging.h"

namespace avbase::base {

Thread::Thread(std::string name)
    : queue_(MakeRefCounted<TaskQueue>()),
      started_(WaitableEvent::ResetPolicy::kManualReset,
               WaitableEvent::InitialState::kNotSignaled) {
  options_.name = std::move(name);
}

Thread::~Thread() {
  Stop();
}

bool Thread::Start() {
  return StartWithOptions(options_);
}

bool Thread::StartWithOptions(const Options& options) {
  if (running_.load(std::memory_order_acquire)) {
    return false;
  }
  options_ = options;
  started_.Reset();

  thread_ = std::thread([this]() { ThreadMain(); });
  thread_id_.store(thread_.get_id(), std::memory_order_release);
  running_.store(true, std::memory_order_release);

  // Wait until the OS thread has named itself and bound the sequence, so that
  // a caller who posts immediately afterwards observes a consistent state.
  started_.Wait();
  return true;
}

void Thread::Stop() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }
  // Quit() first: it flips the flag and wakes the loop, and makes any PostTask
  // racing with shutdown return false instead of enqueueing work that will
  // never run.
  queue_->Quit();
  if (thread_.joinable()) {
    thread_.join();
  }
  running_.store(false, std::memory_order_release);
  thread_id_.store(std::thread::id{}, std::memory_order_release);
  LOG(INFO) << "thread stopped: " << options_.name;
}

void Thread::ThreadMain() {
  PlatformThread::SetName(options_.name);
  PlatformThread::SetCurrentThreadPriority(options_.priority);
  started_.Signal();
  queue_->Run();   // Returns once Quit() is observed.
}

}  // namespace avbase::base
