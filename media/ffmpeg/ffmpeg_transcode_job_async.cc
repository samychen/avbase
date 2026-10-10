// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_transcode_job.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace avbase::media {

// The async half of E3b: it owns the worker thread that drives the synchronous
// Transcode() pump in ffmpeg_transcode_job.cc, plus the cancellation plumbing
// Cancel() reaches through. Split out from the pump so each translation unit
// stays a single concern (and under the C1 length ratchet).

struct TranscodeJob::Impl {
  std::thread worker;
  std::atomic<bool> running{false};
  // |token| is read by Cancel() and written by Start(); the worker holds its
  // own shared_ptr, so this lock only serialises Start() against Cancel().
  std::mutex mu;
  std::shared_ptr<TranscodeCancelToken> token;

  ~Impl() {
    if (worker.joinable()) {
      worker.join();
    }
  }
};

TranscodeJob::TranscodeJob() : impl_(std::make_unique<Impl>()) {}

TranscodeJob::~TranscodeJob() {
  Cancel();
  Wait();
}

bool TranscodeJob::Start(const std::string& input_uri,
                         const TranscodeParams& params,
                         TranscodeProgressCB progress_cb,
                         TranscodeDoneCB done_cb) {
  if (impl_->running.load(std::memory_order_acquire)) {
    return false;  // one job at a time
  }
  // Reap the previous run's thread before reusing the slot. Called from a
  // callback -- i.e. from the worker itself -- joining would deadlock, so a
  // re-entrant Start() is refused instead.
  if (impl_->worker.joinable() &&
      impl_->worker.get_id() == std::this_thread::get_id()) {
    return false;
  }
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  auto token = std::make_shared<TranscodeCancelToken>();
  {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->token = token;
  }
  impl_->running.store(true, std::memory_order_release);
  impl_->worker = std::thread([this, input_uri, params, token,
                               progress_cb = std::move(progress_cb),
                               done_cb = std::move(done_cb)]() mutable {
    Status status = Transcode(input_uri, params, progress_cb, token.get());
    // Clear |running| before done_cb so IsRunning() is already false inside
    // the completion callback.
    impl_->running.store(false, std::memory_order_release);
    if (done_cb) {
      done_cb(std::move(status));
    }
  });
  return true;
}

void TranscodeJob::Cancel() {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->token) {
    impl_->token->Cancel();
  }
}

void TranscodeJob::Wait() {
  if (impl_->worker.joinable() &&
      impl_->worker.get_id() != std::this_thread::get_id()) {
    impl_->worker.join();
  }
}

bool TranscodeJob::IsRunning() const {
  return impl_->running.load(std::memory_order_acquire);
}

}  // namespace avbase::media
