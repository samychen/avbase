// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/throttled_data_source.h"

#include <cstring>
#include <utility>

#include "base/functional/bind.h"
#include "base/time/time.h"

namespace avbase::media::test {

namespace {
// How long one budget-wait slice sleeps before re-checking budget and the
// abort event. Short enough that a test's abort lands promptly, long enough
// that 200 ms of throttling does not spin.
constexpr base::TimeDelta kBudgetSlice = base::Milliseconds(2);
}  // namespace

ThrottledDataSource::ThrottledDataSource(base::scoped_refptr<DataSource> inner,
                                         int bytes_per_second)
    : inner_(std::move(inner)),
      initial_bytes_per_second_(bytes_per_second),
      rate_(bytes_per_second) {
  if (initial_bytes_per_second_ > 0) {
    // One second's worth of burst: the connection is "already full" when the
    // test starts, so the first read does not stall. Everything past the
    // burst paces.
    max_burst_ = static_cast<size_t>(initial_bytes_per_second_);
    budget_ = static_cast<double>(max_burst_);
    last_accrual_ = base::TimeTicks::Now();
  }
}

ThrottledDataSource::~ThrottledDataSource() = default;

void ThrottledDataSource::set_bytes_per_second(int bytes_per_second) {
  // Deliberately does NOT touch budget_. A recovering link resumes at the new
  // rate with whatever the old one left in the pipe; refilling the budget here
  // would hand the consumer a free burst and make recovery pass for the wrong
  // reason.
  rate_.store(bytes_per_second);
}

void ThrottledDataSource::set_max_burst_bytes(size_t bytes) {
  std::scoped_lock scoped(pace_lock_);
  max_burst_ = bytes;
  budget_ = std::min(budget_, static_cast<double>(bytes));
}

void ThrottledDataSource::FailFrom(int64_t offset, MediaError error) {
  std::scoped_lock scoped(fault_lock_);
  fail_from_ = offset;
  failure_ = std::move(error);
}

void ThrottledDataSource::ClearFailure() {
  std::scoped_lock scoped(fault_lock_);
  fail_from_ = -1;
  failure_ = MediaError();
}

int64_t ThrottledDataSource::bytes_served() const {
  std::scoped_lock scoped(stats_lock_);
  return bytes_served_;
}

int ThrottledDataSource::stalls() const {
  std::scoped_lock scoped(stats_lock_);
  return stalls_;
}

void ThrottledDataSource::SetHost(Host* host) {
  // Transparent passthrough: the throttle is not the consumer, it sits
  // between the consumer and the real source.
  inner_->SetHost(host);
}

size_t ThrottledDataSource::TakeBudgetLocked(size_t wanted) {
  // Read through the atomic: set_bytes_per_second() may have changed it since
  // the last call, and pacing to a stale rate is the bug the setter exists to
  // remove.
  const int rate = rate_.load();
  if (rate <= 0) {
    return wanted;
  }
  const base::TimeTicks now = base::TimeTicks::Now();
  const double accrued =
      (now - last_accrual_).InMillisecondsF() * rate / 1000.0;
  last_accrual_ = now;
  budget_ = std::min(budget_ + accrued, static_cast<double>(max_burst_));
  const size_t take =
      static_cast<size_t>(std::min(budget_, static_cast<double>(wanted)));
  budget_ -= static_cast<double>(take);
  return take;
}

ThrottledDataSource::ReadResult
ThrottledDataSource::ReadInternal(int64_t offset, size_t size, uint8_t* data) {
  {
    std::scoped_lock scoped(fault_lock_);
    if (fail_from_ >= 0 && offset >= fail_from_) {
      return base::unexpected(failure_);
    }
  }
  if (aborted_.IsSignaled()) {
    return base::unexpected(
        MediaError::Of(ErrorCode::kSourceReadFailed, "read aborted",
                       "the source was aborted before this read",
                       "reconnect with RetryDataSource instead of reading on"));
  }

  size_t done = 0;
  while (done < size) {
    if (aborted_.IsSignaled()) {
      return base::unexpected(MediaError::Of(
          ErrorCode::kSourceReadFailed, "read aborted",
          "the source was aborted " + std::to_string(done) + " bytes in",
          "reconnect with RetryDataSource instead of reading on"));
    }
    size_t wanted;
    {
      std::scoped_lock scoped(pace_lock_);
      wanted = TakeBudgetLocked(size - done);
      if (wanted == 0 && rate_.load() > 0) {
        std::scoped_lock s2(stats_lock_);
        ++stalls_;
      }
    }
    if (wanted == 0) {
      // No budget. Sleep one slice; the abort event is what makes this
      // return promptly (DataSource::Abort's contract, docs/04 §5.4 Δ15).
      if (aborted_.TimedWait(kBudgetSlice)) {
        continue;  // Re-check: the abort branch above reports the partial.
      }
      continue;
    }
    ReadResult got = inner_->ReadBlocking(offset + static_cast<int64_t>(done),
                                          wanted, data + done);
    if (!got.has_value()) {
      return base::unexpected(got.error());
    }
    if (got.value() == 0) {
      break;  // EOF: the inner source is exhausted.
    }
    done += static_cast<size_t>(got.value());
    {
      std::scoped_lock scoped(stats_lock_);
      bytes_served_ += got.value();
    }
  }
  return static_cast<int>(done);
}

ThrottledDataSource::ReadResult
ThrottledDataSource::ReadBlocking(int64_t offset, size_t size, uint8_t* data) {
  return ReadInternal(offset, size, data);
}

void ThrottledDataSource::Read(
    int64_t offset, size_t size, uint8_t* data,
    base::scoped_refptr<base::TaskRunner> task_runner, ReadCB read_cb) {
  // The async path paces on the caller's thread (test fakes have no worker of
  // their own) and only the reply honours the "never inline" contract.
  ReadResult result = ReadInternal(offset, size, data);
  task_runner->PostTask(FROM_HERE,
                        base::BindOnce(std::move(read_cb), std::move(result)));
}

void ThrottledDataSource::Abort() {
  aborted_.Signal();
  inner_->Abort();
}

bool ThrottledDataSource::GetSize(int64_t* size_out) {
  return inner_->GetSize(size_out);
}

bool ThrottledDataSource::IsStreaming() const {
  return inner_->IsStreaming();
}

void ThrottledDataSource::SetBitrate(int bitrate) {
  inner_->SetBitrate(bitrate);
}

bool ThrottledDataSource::IsSeekable() const {
  return inner_->IsSeekable();
}

}  // namespace avbase::media::test
