// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/player_config.h"

namespace ijkpp {
namespace {

void Add(std::vector<ConfigIssue>* out, std::string field, std::string problem,
         std::string suggestion) {
  out->push_back(ConfigIssue{std::move(field), std::move(problem),
                             std::move(suggestion)});
}

}  // namespace

// Returns every problem at once so a caller can fix them in one pass.
std::vector<ConfigIssue> ValidateConfig(const PlayerConfig& c) {
  std::vector<ConfigIssue> issues;

  if (c.buffer.first_high_water_mark > c.buffer.next_high_water_mark) {
    Add(&issues, "buffer.first_high_water_mark",
        "must be <= buffer.next_high_water_mark",
        "raise first_high_water_mark or lower next_high_water_mark");
  }
  if (c.buffer.next_high_water_mark > c.buffer.last_high_water_mark) {
    Add(&issues, "buffer.next_high_water_mark",
        "must be <= buffer.last_high_water_mark",
        "raise next_high_water_mark or lower last_high_water_mark");
  }
  if (c.buffer.first_high_water_mark.is_negative()) {
    Add(&issues, "buffer.first_high_water_mark", "must not be negative",
        "use a non-negative duration");
  }
  if (c.video.frame_queue_size < 2 || c.video.frame_queue_size > 16) {
    Add(&issues, "video.frame_queue_size", "must be in [2, 16]",
        "3 is the usual value; larger queues use more memory");
  }
  if (c.audio.frame_queue_size < 2) {
    Add(&issues, "audio.frame_queue_size", "must be >= 2", "use 9");
  }
  if (c.audio.startup_volume < 0.0 || c.audio.startup_volume > 1.0) {
    Add(&issues, "audio.startup_volume", "must be in [0.0, 1.0]",
        "note the range changed from ijkplayer's 0..100 (see docs/05 Δ6)");
  }
  if (c.video.max_fps < -1 || c.video.max_fps > 121) {
    Add(&issues, "video.max_fps", "must be in [-1, 121]",
        "-1 disables the cap; 31 is the ijkplayer default");
  }
  if (c.video.max_frame_drop < -1 || c.video.max_frame_drop > 120) {
    Add(&issues, "video.max_frame_drop", "must be in [-1, 120]",
        "-1 disables dropping; 0 is the default");
  }
  if (c.seek.accurate_timeout.is_negative()) {
    Add(&issues, "seek.accurate_timeout", "must not be negative",
        "use base::Seconds(5) unless you have measured a slower device");
  }
  if (c.loop_count == 0) {
    Add(&issues, "loop_count", "0 means 'never play', which is never intended",
        "use 1 for a single pass or -1 for infinite looping");
  }
  if (c.shutdown_timeout < base::Milliseconds(50)) {
    Add(&issues, "shutdown_timeout", "must be >= 50ms",
        "too small a value detaches sequences and leaks threads (see Δ15)");
  }
  if (c.demux.probe_size < 0) {
    Add(&issues, "demux.probe_size", "must not be negative", "use 5 MiB");
  }
  if (c.demux.analyze_duration.is_negative()) {
    Add(&issues, "demux.analyze_duration", "must not be negative",
        "use base::Seconds(5)");
  }
  if (c.video.decoder_preference == DecoderPreference::kHardwareOnly &&
      c.video.hw_codecs == 0) {
    Add(&issues, "video.hw_codecs",
        "kHardwareOnly selected but no hardware codecs are enabled",
        "set hw_codecs to at least one flag, or use kHardwareFirst / kAuto");
  }
  return issues;
}

}  // namespace ijkpp
