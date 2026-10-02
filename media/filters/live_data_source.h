// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_LIVE_DATA_SOURCE_H_
#define AVBASE_MEDIA_FILTERS_LIVE_DATA_SOURCE_H_

#include <atomic>
#include <cstdint>
#include <deque>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/synchronization/atomic_flag.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/task/task_runner.h"
#include "base/threading/thread.h"
#include "media/base/data_source.h"
#include "media/base/media_error.h"
#include "media/media_export.h"

namespace avbase::media {

// A growable byte source for live streams: a producer appends bytes as they
// arrive (network callback, test rig), and the consumer -- normally
// FFmpegDemuxer through the DataSource→AVIOContext bridge -- reads
// sequentially, BLOCKING at the live edge until the producer catches up.
// This is the 2.2 infrastructure item (docs/12): the common prerequisite
// for live-edge chase (2.1) and live-subtitle expiry (6.2), and the only
// automatable stand-in when no real live network source exists.
//
// SEMANTICS.
//   * Streaming, non-seekable: IsSeekable() is false, IsStreaming() true.
//     FFmpeg's mpegts demuxer (the live container) never seeks -- that is
//     what makes this combination automatable.
//   * ReadBlocking(offset, ...) serves bytes from the append-only buffer at
//     |offset|; offsets are absolute positions in the stream. A read with
//     nothing available BLOCKS in abortable slices until the producer
//     appends, Close() marks EOF (reads past the end return 0), or Abort()
//     fires. A read that can be PARTIALLY served returns what is available
//     immediately -- a live consumer must not wait for bytes that may grow.
//   * Close() is the "stream ended" signal (rare for live, but required for
//     clean test teardown and for streams that turn out to be finite).
//
// THREADING. Producer (Append/Close) and consumer (reads, from the demux
// thread via the bridge) are any threads; buffer_ and closed_ are guarded
// by lock_, the blocking wait uses condition_variable slices so Abort()
// always bounds it (Δ15).
class AVBASE_MEDIA_EXPORT LiveDataSource final : public DataSource {
 public:
  LiveDataSource();
  LiveDataSource(const LiveDataSource&) = delete;
  LiveDataSource& operator=(const LiveDataSource&) = delete;
  ~LiveDataSource() override;

  // ---- Producer side (any thread) ------------------------------------------
  void Append(const uint8_t* data, size_t size);
  // No more data will ever be appended; reads past the end return EOF.
  void Close();
  // Bytes appended so far == the current end of the "file".
  size_t appended_size() const;

  // ---- DataSource ----------------------------------------------------------
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

 private:
  // Runs ReadBlocking on the worker thread for async Read(); a live read may
  // block at the edge, and the DataSource contract forbids inline callbacks.
  void WorkerMain();
  ReadResult ReadBlockingImpl(int64_t offset, size_t size, uint8_t* data);

  mutable base::Lock lock_;
  base::ConditionVariable data_cv_ GUARDED_BY(lock_);
  std::vector<uint8_t> buffer_ GUARDED_BY(lock_);
  bool closed_ GUARDED_BY(lock_) = false;

  base::AtomicFlag aborted_;
  base::Thread worker_;
  base::Lock job_lock_;
  base::ConditionVariable job_cv_ GUARDED_BY(job_lock_);
  struct AsyncJob {
    int64_t offset;
    size_t size;
    uint8_t* data;
    base::scoped_refptr<base::TaskRunner> runner;
    ReadCB cb;
  };
  std::deque<AsyncJob> jobs_ GUARDED_BY(job_lock_);
  bool stopped_ GUARDED_BY(job_lock_) = false;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_LIVE_DATA_SOURCE_H_
