// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_URL_DATA_SOURCE_H_
#define AVBASE_MEDIA_FFMPEG_URL_DATA_SOURCE_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/task/task_runner.h"
#include "media/base/data_source.h"
#include "media/base/media_error.h"
#include "media/media_export.h"

struct AVIOContext;

namespace avbase::media::ffmpeg {

// An avio-backed DataSource for network URIs (http/https today). This is the
// component that lets the RETRY layer exist on the network path at all: the
// FFmpeg protocol layer can reconnect itself mid-stream, but a connection
// reset during connect or a hard reset mid-transfer surfaces as a read error
// on OUR side -- and only a DataSource decorator (RetryDataSource) wrapping
// THIS can turn that into a fresh connection. Before this existed,
// RetryDataSource was implemented, tested, and wired to nothing.
//
// LIFETIME/THREADING: ReadBlocking runs on the demuxer's thread and may block
// for the network duration; Abort() flips an atomic that the avio interrupt
// callback observes, so a blocked read returns AVERROR_EXIT promptly (Δ15).
// The async Read() runs on a private worker like RetryDataSource's: the
// callback is posted to |task_runner|, never inline.
class AVBASE_MEDIA_EXPORT UrlDataSource final : public media::DataSource {
 public:
  explicit UrlDataSource(std::string uri);
  UrlDataSource(const UrlDataSource&) = delete;
  UrlDataSource& operator=(const UrlDataSource&) = delete;
  ~UrlDataSource() override;

  // media::DataSource:
  void SetHost(Host* host) override;
  void Read(int64_t offset, size_t size, uint8_t* data,
            base::scoped_refptr<base::TaskRunner> task_runner,
            ReadCB read_cb) override;
  ReadResult ReadBlocking(int64_t offset, size_t size, uint8_t* data) override;
  void Abort() override;
  bool GetSize(int64_t* size_out) override;
  bool IsStreaming() const override;
  void SetBitrate(int bitrate) override;
  bool IsSeekable() const override;

  // Observed by the avio interrupt callback (registered at open). Public
  // only because the callback is a file-local free function.
  bool IsAbortedForInterrupt() const {
    return aborted_.load(std::memory_order_relaxed);
  }

 private:
  // Opens the AVIOContext on first use (the demuxer opens its probe on its
  // own thread; opening here keeps the ctor infallible and lets open errors
  // surface as read errors with the URI in the message).
  media::DataSource::ReadResult EnsureOpen();
  void CloseLocked();

  const std::string uri_;
  mutable std::mutex lock_;
  AVIOContext* avio_ = nullptr;  // GUARDED_BY(lock_) for the pointer itself.
  std::atomic<bool> aborted_{false};
  bool size_known_ = false;  // GUARDED_BY(lock_)
  int64_t size_ = -1;        // GUARDED_BY(lock_)
  bool seekable_ = false;    // GUARDED_BY(lock_)
  int bitrate_ = 0;
  // The async-read machinery: one lazily created worker draining a job
  // queue, so Read()'s callback is posted (never inline) while the blocking
  // avio call happens off the caller's thread.
  void WorkerMain();
  std::mutex pending_lock_;
  std::condition_variable pending_cv_;
  std::deque<avbase::base::OnceClosure> pending_jobs_;
  bool worker_shutdown_ = false;
  std::unique_ptr<std::thread> worker_;
};

}  // namespace avbase::media::ffmpeg

#endif  // AVBASE_MEDIA_FFMPEG_URL_DATA_SOURCE_H_
