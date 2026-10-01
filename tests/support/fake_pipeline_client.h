// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// A recording Pipeline::Client for pipeline-level tests. Every callback may
// arrive on the media sequence while the test thread polls the accessors, so
// the event log is under a mutex and the accessors return snapshots rather
// than references.

#ifndef IJKPP_TESTS_SUPPORT_FAKE_PIPELINE_CLIENT_H_
#define IJKPP_TESTS_SUPPORT_FAKE_PIPELINE_CLIENT_H_

#include <mutex>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "media/base/media_error.h"
#include "media/base/pipeline.h"
#include "media/base/renderer_client.h"

namespace ijkpp::media::test {

class FakePipelineClient final : public Pipeline::Client {
 public:
  FakePipelineClient() = default;

  // ---- Test-side snapshots (thread-safe) ---------------------------------
  bool Started() const {
    // The Start() completion contract (pipeline.h): first duration change or
    // buffering state change, or an error.
    std::scoped_lock scoped(lock_);
    return duration_seen_ || buffering_count_ > 0 || error_ != MediaError();
  }
  bool HaveMetadata() const {
    std::scoped_lock scoped(lock_);
    return have_metadata_;
  }
  bool HaveEnough() const {
    std::scoped_lock scoped(lock_);
    return have_enough_;
  }
  bool HasError() const {
    std::scoped_lock scoped(lock_);
    return error_ != MediaError();
  }
  MediaError error() const {
    std::scoped_lock scoped(lock_);
    return error_;
  }
  bool ended() const {
    std::scoped_lock scoped(lock_);
    return ended_;
  }
  int buffering_count() const {
    std::scoped_lock scoped(lock_);
    return buffering_count_;
  }
  int waiting_count() const {
    std::scoped_lock scoped(lock_);
    return waiting_count_;
  }
  base::TimeDelta duration() const {
    std::scoped_lock scoped(lock_);
    return duration_;
  }

  // One line per event, for test failure messages.
  std::string EventLog() const;

  // Pipeline::Client.
  void OnError(MediaError error) override;
  void OnEnded() override;
  void OnDurationChange(base::TimeDelta duration) override;
  void OnBufferingStateChange(BufferingState state,
                              base::TimeDelta memory_usage) override;
  void OnWaiting(WaitingReason reason) override;
  void OnStatisticsUpdate(const PipelineStatistics& stats) override;
  void OnVideoConfigChange(const VideoDecoderConfig& config) override;

 private:
  void Record(const std::string& line);

  mutable std::mutex lock_;
  std::vector<std::string> events_;
  MediaError error_;
  base::TimeDelta duration_;
  bool duration_seen_ = false;
  bool have_metadata_ = false;
  bool have_enough_ = false;
  bool ended_ = false;
  int buffering_count_ = 0;
  int waiting_count_ = 0;
};

}  // namespace ijkpp::media::test

#endif  // IJKPP_TESTS_SUPPORT_FAKE_PIPELINE_CLIENT_H_
