// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/playback_stats.h"

#include <cstdio>

namespace ijkpp {
namespace {

std::string Num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", v);
  return buf;
}

}  // namespace

std::string PlaybackStats::ToJson() const {
  std::string out = "{";
  out += "\"video_decode_fps\":" + Num(video_decode_fps) + ",";
  out += "\"video_output_fps\":" + Num(video_output_fps) + ",";
  out += "\"playback_rate\":" + Num(playback_rate) + ",";
  out += "\"av_diff_us\":" + std::to_string(av_diff.InMicroseconds()) + ",";
  out += "\"drop_frame_rate\":" + Num(drop_frame_rate) + ",";
  out += "\"frames_presented\":" + std::to_string(frames_presented) + ",";
  out += "\"frames_dropped\":" + std::to_string(frames_dropped) + ",";
  out += "\"frames_dropped_late\":" + std::to_string(frames_dropped_late) + ",";
  out += "\"frames_repeated\":" + std::to_string(frames_repeated) + ",";
  out += "\"audio_glitches\":" + std::to_string(audio_glitches.total_glitches) + ",";
  out += "\"audio_xruns\":" + std::to_string(audio_glitches.xruns) + ",";
  out += "\"video_cached_ms\":" + std::to_string(video.cached_duration.InMilliseconds()) + ",";
  out += "\"audio_cached_ms\":" + std::to_string(audio.cached_duration.InMilliseconds()) + ",";
  out += "\"buffering_count\":" + std::to_string(buffering.buffering_count) + ",";
  out += "\"hwm_step\":" + std::to_string(buffering.current_hwm_step) + ",";
  out += "\"bit_rate\":" + std::to_string(bit_rate) + ",";
  out += "\"tcp_speed\":" + std::to_string(tcp_speed) + ",";
  out += "\"first_frame_ms\":" + std::to_string(first_frame_latency.InMilliseconds()) + ",";
  out += "\"stages\":{";
  out += "\"open_input_ms\":" + std::to_string(stages.prepare_to_open_input.InMilliseconds()) + ",";
  out += "\"find_stream_info_ms\":" + std::to_string(stages.open_input_to_stream_info.InMilliseconds()) + ",";
  out += "\"decoder_open_ms\":" + std::to_string(stages.stream_info_to_decoder_open.InMilliseconds()) + ",";
  out += "\"first_frame_ms\":" + std::to_string(stages.first_frame_to_presented.InMilliseconds());
  out += "},";
  out += "\"heap_bytes\":" + std::to_string(heap_bytes_estimate);
  out += "}";
  return out;
}

}  // namespace ijkpp
