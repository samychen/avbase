// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/mock_renderer_client.h"

#include <utility>

namespace avbase::media::test {

int FakeRendererClient::Count(std::string_view kind) const {
  int count = 0;
  for (const Event& event : events_) {
    if (event.kind == kind) {
      ++count;
    }
  }
  return count;
}

int FakeRendererClient::IndexOf(std::string_view kind) const {
  for (size_t i = 0; i < events_.size(); ++i) {
    if (events_[i].kind == kind) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void FakeRendererClient::OnError(MediaError error) {
  last_error_ = std::move(error);
  events_.push_back({"error", base::TimeDelta()});
}

void FakeRendererClient::OnEnded() {
  events_.push_back({"ended", base::TimeDelta()});
}

void FakeRendererClient::OnBufferingStateChange(BufferingState state,
                                                base::TimeDelta memory_usage) {
  last_buffering_state_ = state;
  events_.push_back({"buffering", memory_usage});
}

void FakeRendererClient::OnWaiting(WaitingReason /*reason*/) {
  events_.push_back({"waiting", base::TimeDelta()});
}

void FakeRendererClient::OnDurationChange(base::TimeDelta duration) {
  last_duration_ = duration;
  events_.push_back({"duration", duration});
}

void FakeRendererClient::OnStatisticsUpdate(const PipelineStatistics& stats) {
  last_stats_ = stats;
  events_.push_back({"statistics", base::TimeDelta()});
}

void FakeRendererClient::OnVideoConfigChange(const VideoDecoderConfig& config) {
  last_video_config_ = config;
  events_.push_back({"video_config", base::TimeDelta()});
}

void FakeRendererClient::OnTimedText(const TimedTextCue& cue) {
  cues_.push_back(cue);
  events_.push_back({"timed_text", cue.pts});
}

void FakeRendererClient::OnAudioOutputDeviceChanged(
    const std::string& /*device_id*/, bool /*is_default*/,
    OutputDeviceStatus /*status*/) {
  events_.push_back({"audio_output_device", base::TimeDelta()});
}

base::scoped_refptr<base::SequencedTaskRunner>
FakeRendererClient::GetOverlayTaskRunner() {
  ++overlay_runner_calls_;
  return nullptr;
}

}  // namespace avbase::media::test
