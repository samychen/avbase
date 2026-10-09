// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/retry_data_source.h"

#include <chrono>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/default_tick_clock.h"

namespace avbase::media {
namespace {

}  // namespace

RetryDataSource::RetryDataSource(base::scoped_refptr<DataSource> inner,
                                 Config config)
    : inner_(std::move(inner)),
      config_(config),
      worker_("avbase-retry-src"),
      pending_cv_(&pending_lock_) {
  CHECK(inner_);
  worker_.Start();
  worker_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&RetryDataSource::WorkerMain, base::Unretained(this)));
}

RetryDataSource::~RetryDataSource() {
  {
    base::AutoLock scoped(pending_lock_);
    stopped_ = true;
  }
  pending_cv_.Signal();
  aborted_.Set();
  inner_->Abort();
  worker_.Stop();
}

void RetryDataSource::SetHost(Host* host) {
  inner_->SetHost(host);
}

bool RetryDataSource::GetSize(int64_t* size_out) {
  return inner_->GetSize(size_out);
}

bool RetryDataSource::IsStreaming() const {
  return inner_->IsStreaming();
}

void RetryDataSource::SetBitrate(int bitrate) {
  inner_->SetBitrate(bitrate);
}

bool RetryDataSource::IsSeekable() const {
  return inner_->IsSeekable();
}

void RetryDataSource::Abort() {
  aborted_.Set();
  inner_->Abort();
}

DataSource::ReadResult
RetryDataSource::ReadBlocking(int64_t offset, size_t size, uint8_t* data) {
  int failures = 0;
  while (true) {
    if (aborted_.IsSet()) {
      return Err(ErrorCode::kAborted, "the retry source was aborted",
                 "offset = " + std::to_string(offset),
                 "this follows Stop(); no action needed");
    }
    ReadResult result = inner_->ReadBlocking(offset, size, data);
    if (result.has_value()) {
      return result;
    }
    // A success resets the streak; only consecutive failures spend the
    // budget, so intermittent blips never turn into a give-up.
    if (++failures > config_.max_retries) {
      return result;
    }
    const base::TimeTicks deadline =
        base::TimeTicks::Now() + config_.retry_delay;
    while (base::TimeTicks::Now() < deadline) {
      if (aborted_.IsSet()) {
        return Err(ErrorCode::kAborted, "the retry source was aborted",
                   "offset = " + std::to_string(offset),
                   "this follows Stop(); no action needed");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

void RetryDataSource::Read(int64_t offset, size_t size, uint8_t* data,
                           base::scoped_refptr<base::TaskRunner> task_runner,
                           ReadCB read_cb) {
  base::AutoLock scoped(pending_lock_);
  if (stopped_) {
    task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(read_cb), Err(ErrorCode::kAborted,
                                               "the retry source was torn down",
                                               {}, "no action needed")));
    return;
  }
  jobs_.push_back(
      AsyncJob{offset, size, data, std::move(task_runner), std::move(read_cb)});
  pending_cv_.Signal();
}

void RetryDataSource::WorkerMain() {
  while (true) {
    AsyncJob job;
    {
      base::AutoLock scoped(pending_lock_);
      while (jobs_.empty() && !stopped_) {
        pending_cv_.Wait();
      }
      if (stopped_) {
        return;
      }
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    ReadResult result = ReadBlocking(job.offset, job.size, job.data);
    job.runner->PostTask(FROM_HERE,
                         base::BindOnce(std::move(job.cb), std::move(result)));
  }
}

}  // namespace avbase::media
