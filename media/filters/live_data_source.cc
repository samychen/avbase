// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/live_data_source.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"

namespace avbase::media {
namespace {

// Abort() must bound every wait (Δ15); the edge wait polls in slices of
// this size so an abort or an append is noticed promptly.
constexpr std::chrono::milliseconds kEdgeWaitSlice(10);

}  // namespace

LiveDataSource::LiveDataSource()
    : data_cv_(&lock_), worker_("ijkpp-live-src"), job_cv_(&job_lock_) {
  worker_.Start();
  worker_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&LiveDataSource::WorkerMain, base::Unretained(this)));
}

LiveDataSource::~LiveDataSource() {
  {
    base::AutoLock scoped(job_lock_);
    stopped_ = true;
  }
  job_cv_.Signal();
  Close();
  Abort();
  worker_.Stop();
}

void LiveDataSource::Append(const uint8_t* data, size_t size) {
  if (size == 0) {
    return;
  }
  {
    base::AutoLock scoped(lock_);
    buffer_.insert(buffer_.end(), data, data + size);
  }
  data_cv_.Broadcast();
}

size_t LiveDataSource::appended_size() const {
  base::AutoLock scoped(lock_);
  return buffer_.size();
}

void LiveDataSource::Close() {
  {
    base::AutoLock scoped(lock_);
    closed_ = true;
  }
  data_cv_.Broadcast();
}

void LiveDataSource::SetHost(Host* host) {
  // A live byte source has no demuxer-facing host needs; the bridge owns
  // the demuxer side. Kept for the contract.
}

DataSource::ReadResult
LiveDataSource::ReadBlockingImpl(int64_t offset, size_t size, uint8_t* data) {
  if (offset < 0) {
    return Err(ErrorCode::kInvalidArgument, "negative read offset",
               "offset = " + std::to_string(offset),
               "a streaming source reads sequentially from 0");
  }
  while (true) {
    {
      base::AutoLock scoped(lock_);
      const size_t available =
          buffer_.size() > static_cast<size_t>(offset)
              ? buffer_.size() - static_cast<size_t>(offset)
              : 0;
      if (available > 0) {
        // Partial serves are the live contract: deliver what exists now.
        const size_t n = std::min(available, size);
        std::memcpy(data, buffer_.data() + offset, n);
        return static_cast<int>(n);
      }
      if (closed_) {
        return 0;  // EOF: the stream was finite after all.
      }
      if (aborted_.IsSet()) {
        return Err(ErrorCode::kAborted, "the live source was aborted",
                   "offset = " + std::to_string(offset),
                   "this follows Stop(); no action needed");
      }
    }
    // At the live edge: the producer has nothing new yet. Wait in slices so
    // Abort()/Close()/Append() are all noticed promptly.
    std::this_thread::sleep_for(kEdgeWaitSlice);
  }
}

DataSource::ReadResult LiveDataSource::ReadBlocking(int64_t offset, size_t size,
                                                    uint8_t* data) {
  return ReadBlockingImpl(offset, size, data);
}

void LiveDataSource::Abort() {
  aborted_.Set();
  data_cv_.Broadcast();
}

bool LiveDataSource::GetSize(int64_t* size_out) {
  // The size grows as the producer appends; reported honestly, but a live
  // consumer should rely on short reads, not on polling the size.
  base::AutoLock scoped(lock_);
  *size_out = static_cast<int64_t>(buffer_.size());
  return true;
}

bool LiveDataSource::IsStreaming() const {
  return true;
}

void LiveDataSource::SetBitrate(int bitrate) {}

bool LiveDataSource::IsSeekable() const {
  return false;  // The defining property of the live case.
}

void LiveDataSource::Read(int64_t offset, size_t size, uint8_t* data,
                          base::scoped_refptr<base::TaskRunner> task_runner,
                          ReadCB read_cb) {
  base::AutoLock scoped(job_lock_);
  if (stopped_) {
    task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(read_cb),
                       Err(ErrorCode::kAborted, "the live source was torn down",
                           {}, "no action needed")));
    return;
  }
  jobs_.push_back(
      AsyncJob{offset, size, data, std::move(task_runner), std::move(read_cb)});
  job_cv_.Signal();
}

void LiveDataSource::WorkerMain() {
  while (true) {
    AsyncJob job;
    {
      base::AutoLock scoped(job_lock_);
      while (jobs_.empty() && !stopped_) {
        job_cv_.Wait();
      }
      if (stopped_) {
        return;
      }
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    ReadResult result = ReadBlockingImpl(job.offset, job.size, job.data);
    job.runner->PostTask(FROM_HERE,
                         base::BindOnce(std::move(job.cb), std::move(result)));
  }
}

}  // namespace avbase::media
