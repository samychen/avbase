// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_RETRY_DATA_SOURCE_H_
#define AVBASE_MEDIA_FILTERS_RETRY_DATA_SOURCE_H_

#include <atomic>
#include <deque>

#include "base/memory/scoped_refptr.h"
#include "base/synchronization/atomic_flag.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/task/task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "media/base/data_source.h"
#include "media/base/media_error.h"
#include "media/media_export.h"

namespace avbase::media {

// A DataSource decorator that retries a failed inner read instead of
// reporting it upward -- the M9 answer to "the network died mid-stream"
// (docs/08; ijkplayer's reconnect_streamed/delay_max options, Δ11's
// structured replacement). The fault-injection rig (ThrottledDataSource::
// FailFrom) is what makes the retry path observable in tests.
//
// WHAT "RETRY" MEANS. ReadBlocking re-issues the SAME (offset, size) request
// up to |max_retries| times, sleeping |retry_delay| between attempts in
// abortable slices -- a stalled network must not park Stop() (Δ15). After
// the budget is spent the LAST error is reported; the caller decides
// whether that is fatal. A successful attempt resets the failure streak, so
// intermittent blips never accumulate into a give-up.
//
// THREADING. ReadBlocking runs on the demux thread (D3); the async Read()
// runs on a private worker and posts its result to |task_runner| -- never
// inline (the DataSource contract). Abort() stops both paths promptly and
// forwards to the inner source.
class AVBASE_MEDIA_EXPORT RetryDataSource final : public DataSource {
 public:
  struct Config {
    int max_retries{3};
    base::TimeDelta retry_delay{base::Milliseconds(100)};
  };

  RetryDataSource(base::scoped_refptr<DataSource> inner, Config config);
  RetryDataSource(const RetryDataSource&) = delete;
  RetryDataSource& operator=(const RetryDataSource&) = delete;
  ~RetryDataSource() override;

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
  void WorkerMain();

  base::scoped_refptr<DataSource> inner_;
  const Config config_;

  base::AtomicFlag aborted_;
  // The async Read queue: one worker drains (offset, size, data, runner, cb)
  // tuples so a blocked read never occupies the caller. The retry loop for
  // async reads runs on that worker too; its abortable sleeps end early via
  // aborted_ + the inner source's own Abort().
  struct AsyncJob {
    int64_t offset;
    size_t size;
    uint8_t* data;
    base::scoped_refptr<base::TaskRunner> runner;
    ReadCB cb;
  };

  base::Thread worker_;
  base::Lock pending_lock_;
  base::ConditionVariable pending_cv_ GUARDED_BY(pending_lock_);
  std::deque<AsyncJob> jobs_ GUARDED_BY(pending_lock_);
  bool stopped_ GUARDED_BY(pending_lock_) = false;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_RETRY_DATA_SOURCE_H_
