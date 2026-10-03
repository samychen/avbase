// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Records everything a renderer reports upward. media/base/renderer_client.h
// names this file ("tests/support/mock_renderer_client, not written yet"): the
// interface has nine pure virtuals, so writing it once here is the difference
// between one fake and nine.
//
// Hand-written rather than gmock-based (the test targets link GTest::gtest_main
// only), and deliberately an ordered event log rather than a set of counters:
// the tenth round's init-callback bug was an ordering bug, and "OnEnded was
// called" would not have caught it.

#ifndef AVBASE_TESTS_SUPPORT_MOCK_RENDERER_CLIENT_H_
#define AVBASE_TESTS_SUPPORT_MOCK_RENDERER_CLIENT_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "media/base/decoder_config.h"
#include "media/base/media_error.h"
#include "media/base/timed_text.h"
#include "media/base/renderer_client.h"

namespace avbase::media::test {

class FakeRendererClient final : public RendererClient {
 public:
  FakeRendererClient() = default;

  // One row per call, in arrival order. |value| carries whatever the call
  // carried that the suites assert on: memory usage for a buffering change,
  // the duration for OnDurationChange, zero otherwise.
  struct Event {
    std::string_view kind;
    base::TimeDelta value;
  };

  const std::vector<Event>& events() const { return events_; }
  bool Has(std::string_view kind) const { return Count(kind) > 0; }
  int Count(std::string_view kind) const;
  // Index of the first |kind| event in |events()|, or -1 when it never came.
  int IndexOf(std::string_view kind) const;

  const MediaError& last_error() const { return last_error_; }
  const PipelineStatistics& last_stats() const { return last_stats_; }
  BufferingState last_buffering_state() const {
    return last_buffering_state_;
  }
  base::TimeDelta last_duration() const { return last_duration_; }
  const VideoDecoderConfig& last_video_config() const {
    return last_video_config_;
  }
  // GetOverlayTaskRunner() is polled rather than reported, so it is counted
  // instead of appended to the event log.
  int overlay_runner_calls() const { return overlay_runner_calls_; }
  // Cues the renderer actually delivered, in arrival order. Needed because
  // "the live-cue policy dropped it" and "the policy never ran" look identical
  // from anywhere else: the only evidence either way is what reached here.
  const std::vector<TimedTextCue>& cues() const { return cues_; }

  // RendererClient.
  void OnError(MediaError error) override;
  void OnEnded() override;
  void OnBufferingStateChange(BufferingState state,
                              base::TimeDelta memory_usage) override;
  void OnWaiting(WaitingReason reason) override;
  void OnDurationChange(base::TimeDelta duration) override;
  void OnStatisticsUpdate(const PipelineStatistics& stats) override;
  void OnVideoConfigChange(const VideoDecoderConfig& config) override;
  void OnTimedText(const TimedTextCue& cue) override;
  void OnAudioOutputDeviceChanged(const std::string& device_id,
                                  bool is_default,
                                  OutputDeviceStatus status) override;
  // nullptr is a legal answer ("no such sequence"), and the renderer must then
  // avoid the work rather than run it inline.
  base::scoped_refptr<base::SequencedTaskRunner> GetOverlayTaskRunner()
      override;

 private:
  std::vector<Event> events_;
  MediaError last_error_;
  std::vector<TimedTextCue> cues_;
  PipelineStatistics last_stats_;
  BufferingState last_buffering_state_ = BufferingState::kHaveNothing;
  base::TimeDelta last_duration_;
  VideoDecoderConfig last_video_config_;
  int overlay_runner_calls_ = 0;
};

}  // namespace avbase::media::test

#endif  // AVBASE_TESTS_SUPPORT_MOCK_RENDERER_CLIENT_H_
