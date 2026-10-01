// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_PUBLIC_PLAYBACK_STATS_H_
#define AVBASE_PLAYER_PUBLIC_PLAYBACK_STATS_H_

#include <stdint.h>

#include <string>

#include "base/time/time.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/media_types.h"
#include "player/public/player_export.h"

namespace avbase {

// Replaces the 30+ FFP_PROP_* integer properties that ijkplayer callers had to
// poll one at a time, each taking a mutex. One call, one immutable snapshot.
// See docs/05 table 6 for the field-by-field mapping.
struct AVBASE_PLAYER_EXPORT BufferStats {
  base::TimeDelta cached_duration;
  size_t cached_bytes{0};
  size_t cached_buffers{0};
};

struct AVBASE_PLAYER_EXPORT AsyncStats {
  base::TimeDelta buf_backwards;
  base::TimeDelta buf_forwards;
  base::TimeDelta buf_capacity;
};

struct AVBASE_PLAYER_EXPORT CacheStats {
  int64_t physical_pos{0};
  int64_t file_forwards{0};
  int64_t file_pos{0};
  int64_t count_bytes{0};
  int64_t logical_file_size{0};
};

struct AVBASE_PLAYER_EXPORT BufferingStats {
  uint64_t buffering_count{0};
  base::TimeDelta total_buffering_time;
  base::TimeDelta last_buffering_time;
  int current_hwm_step{0};
  base::TimeDelta active_high_water_mark;
};

// First-frame latency decomposition. ijkplayer emitted FFP_MSG_OPEN_INPUT /
// FIND_STREAM_INFO / COMPONENT_OPEN for exactly this purpose but left the
// caller to stitch the timestamps together.
struct AVBASE_PLAYER_EXPORT StageTimings {
  base::TimeDelta prepare_to_open_input;
  base::TimeDelta open_input_to_stream_info;
  base::TimeDelta stream_info_to_decoder_open;
  base::TimeDelta decoder_open_to_first_packet;
  base::TimeDelta first_packet_to_first_frame;
  base::TimeDelta first_frame_to_presented;
  base::TimeDelta total_first_frame;
  base::TimeDelta audio_first_rendered;
};

struct AVBASE_PLAYER_EXPORT PlaybackStats {
  double video_decode_fps{0.0};
  double video_output_fps{0.0};
  double playback_rate{1.0};
  double volume{1.0};
  double drop_frame_rate{0.0};
  base::TimeDelta av_delay;
  base::TimeDelta av_diff;

  int selected_video_stream{-1};
  int selected_audio_stream{-1};
  int selected_text_stream{-1};
  media::VideoDecoderType video_decoder{media::VideoDecoderType::kUnknown};
  media::AudioDecoderType audio_decoder{media::AudioDecoderType::kUnknown};

  BufferStats video;
  BufferStats audio;
  BufferingStats buffering;

  int64_t bit_rate{0};
  int64_t tcp_speed{0};
  AsyncStats async;
  int64_t traffic_byte_count{0};
  CacheStats cache;
  base::TimeDelta latest_seek_load_duration;

  uint64_t frames_presented{0};
  uint64_t frames_dropped{0};
  uint64_t frames_dropped_late{0};
  uint64_t frames_dropped_fps{0};
  uint64_t frames_repeated{0};
  // Underrun accounting straight from the audio sink. Turns "sometimes it
  // crackles" into a number you can alert on.
  media::AudioGlitchInfo audio_glitches;

  StageTimings stages;
  base::TimeDelta first_frame_latency;
  size_t heap_bytes_estimate{0};
  uint64_t events_dropped_by_overflow{0};

  std::string ToJson() const;
};

}  // namespace avbase

#endif  // AVBASE_PLAYER_PUBLIC_PLAYBACK_STATS_H_
