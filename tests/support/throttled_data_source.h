// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_TESTS_SUPPORT_THROTTLED_DATA_SOURCE_H_
#define AVBASE_TESTS_SUPPORT_THROTTLED_DATA_SOURCE_H_

#include <stdint.h>

#include <atomic>
#include <cstddef>
#include <mutex>

#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "media/base/data_source.h"
#include "media/base/media_error.h"

namespace avbase::media::test {

// A DataSource that serves its inner source at a capped rate, with fault
// injection. This is the M9 test rig (docs/08): the three-tier HWM can only
// be *observed* through a source that stalls, the retry policy needs a source
// that fails mid-stream, and the live-latency policy needs one that keeps
// producing. Real media files via FFmpegDemuxer are none of those things.
//
// Pacing is wall-clock (the demux thread's ReadBlocking sleeps in short
// slices while it waits for budget), so a test reading N bytes at rate R
// takes about N/R wall time. Rates in tests are small on purpose: 50 KB/s
// over a 10 KB file is 200 ms, not minutes.
class ThrottledDataSource final : public DataSource {
 public:
  // |bytes_per_second| <= 0 means passthrough: no pacing at all. |inner|
  // supplies the bytes (usually a MemoryDataSource over the test media).
  ThrottledDataSource(base::scoped_refptr<DataSource> inner,
                      int bytes_per_second);
  ~ThrottledDataSource() override;

  // ---- fault injection -----------------------------------------------------
  // Every read at or beyond |offset| fails with |error| until ClearFailure().
  // This is the "network died mid-stream" event RetryDataSource must recover
  // from; arming it does not touch reads before the offset.
  void FailFrom(int64_t offset, MediaError error);
  void ClearFailure();

  // Caps the instant-serve burst. The default is one second's worth of the
  // rate (a full connection window); set it smaller to make the throttle
  // bite inside a test-sized file -- with the default, a 20 KB file at
  // 50 KB/s finishes inside the initial burst and the throttle never
  // throttles.
  void set_max_burst_bytes(size_t bytes);

  // Changes the delivery rate WHILE the source is open.
  //
  // WHY THIS EXISTS, because a constant rate cannot express the thing the test
  // is for. A fixed rate either exceeds consumption (the queues refill, so
  // kHaveNothing never fires) or falls below it (the queues drain and never
  // recover, so kHaveEnough never fires). Neither produces a STALL-RECOVER
  // cycle, which is the thing M9's DoD actually asks for.
  //
  // What does produce it is what happens on a real network: the link goes
  // slow, playback starves, the link comes back, playback resumes. So the
  // rate moves mid-stream, and the test drives it.
  //
  // The budget is not reset, deliberately: a link that recovers does not hand
  // the viewer a fresh connection's worth of buffered bytes, and resetting it
  // would make recovery trivially easy for the wrong reason.
  void set_bytes_per_second(int bytes_per_second);

  // ---- test observation ----------------------------------------------------
  int64_t bytes_served() const;
  // How many times a read had to wait for budget to accrue (i.e. the throttle
  // actually throttled).
  int stalls() const;

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
  // Takes |pace_lock_|. Accrues budget since the last call and returns how
  // many bytes may be served now.
  size_t TakeBudgetLocked(size_t wanted);
  // The blocking read loop, shared by Read() and ReadBlocking().
  ReadResult ReadInternal(int64_t offset, size_t size, uint8_t* data);

  base::scoped_refptr<DataSource> inner_;
  // The rate is read through rate_ (settable at runtime); this is the
  // constructor's initial value.
  const int initial_bytes_per_second_;

  // Pace state. |budget_| is bytes available right now; it accrues with wall
  // time and is capped at |max_burst_| so a slow consumer cannot hoard an
  // hour of budget and then serve a file instantly.
  //
  // |rate_| is atomic because set_bytes_per_second() is called from the test
  // thread while the pacing runs on the demux thread; everything else stays
  // under |pace_lock_|.
  std::mutex pace_lock_;
  double budget_ = 0.0;
  base::TimeTicks last_accrual_;
  size_t max_burst_ = 0;
  std::atomic<int> rate_{0};

  // Fault state.
  std::mutex fault_lock_;
  int64_t fail_from_ = -1;  // -1 = disarmed
  MediaError failure_;

  // Abort. Sticky by design, matching the DataSource contract: Abort() bounds
  // Stop(), and a reconnect builds a fresh source rather than un-aborting a
  // dead one.
  base::WaitableEvent aborted_{base::WaitableEvent::ResetPolicy::kManualReset,
                               base::WaitableEvent::InitialState::kNotSignaled};

  // Counters.
  mutable std::mutex stats_lock_;
  int64_t bytes_served_ = 0;
  int stalls_ = 0;
};

}  // namespace avbase::media::test

#endif  // AVBASE_TESTS_SUPPORT_THROTTLED_DATA_SOURCE_H_
