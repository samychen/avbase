// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/fake_pipeline_client.h"

namespace ijkpp::media::test {

void FakePipelineClient::Record(const std::string& line) {
  std::scoped_lock scoped(lock_);
  events_.push_back(line);
}

std::string FakePipelineClient::EventLog() const {
  std::scoped_lock scoped(lock_);
  std::string out;
  for (const std::string& event : events_) {
    out += "  " + event + "\n";
  }
  return out;
}

void FakePipelineClient::OnError(MediaError error) {
  {
    std::scoped_lock scoped(lock_);
    error_ = error;
  }
  Record("error: " + error.ToString());
}

void FakePipelineClient::OnEnded() {
  std::scoped_lock scoped(lock_);
  ended_ = true;
  events_.push_back("ended");
}

void FakePipelineClient::OnDurationChange(base::TimeDelta duration) {
  {
    std::scoped_lock scoped(lock_);
    duration_ = duration;
    duration_seen_ = true;
  }
  Record("duration: " + std::to_string(duration.InMilliseconds()) + "ms");
}

void FakePipelineClient::OnBufferingStateChange(BufferingState state,
                                                base::TimeDelta memory_usage) {
  {
    std::scoped_lock scoped(lock_);
    ++buffering_count_;
    have_metadata_ = have_metadata_ || state == BufferingState::kHaveMetadata;
    have_enough_ = have_enough_ || state == BufferingState::kHaveEnough;
  }
  Record("buffering: state=" + std::to_string(static_cast<int>(state)) +
         " bytes=" + std::to_string(memory_usage.InMilliseconds()));
}

void FakePipelineClient::OnWaiting(WaitingReason /*reason*/) {
  std::scoped_lock scoped(lock_);
  ++waiting_count_;
  events_.push_back("waiting");
}

void FakePipelineClient::OnStatisticsUpdate(
    const PipelineStatistics& /*stats*/) {}

void FakePipelineClient::OnVideoConfigChange(
    const VideoDecoderConfig& /*config*/) {}

}  // namespace ijkpp::media::test
