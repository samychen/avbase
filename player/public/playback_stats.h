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
#include "media/base/render_perf_stats.h"
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
  // The four reasons above partition frames_dropped: late + fps + stale_serial
  // + accurate_seek == frames_dropped. They are reported separately because
  // "the pipeline is behind" and "a policy limit is thinning the output" call
  // for opposite reactions, and the total alone cannot tell them apart.
  uint64_t frames_dropped_late{0};
  uint64_t frames_dropped_fps{0};
  uint64_t frames_dropped_stale_serial{0};
  uint64_t frames_dropped_accurate_seek{0};
  uint64_t frames_repeated{0};
  // Frames handed to the display layer that it refused because it had nowhere
  // to draw them (no surface, a surface of the wrong kind). A subset of
  // frames_presented, NOT a part of frames_dropped -- see the field comment in
  // media/base/renderer_client.h for why the two are different events.
  uint64_t frames_dropped_by_sink{0};
  // Frames that reached the display layer and could not be shown: upload or
  // present failed, or a hardware frame could not be imported. Nonzero here is
  // a display-path fault, not a slow decode.
  uint64_t frames_submit_failures{0};
  // The rate this display layer actually achieved: the average spacing of its
  // own presents, in Hz. Unlike the display interval a sink is constructed
  // with, this is measured from what happened, so a value below the panel's
  // nominal rate means presents are not arriving every interval (starved, or
  // paced slower) -- not that the display is slow. 0 until one is measured.
  double measured_refresh_hz{0.0};
  // Underrun accounting straight from the audio sink. Turns "sometimes it
  // crackles" into a number you can alert on.
  media::AudioGlitchInfo audio_glitches;

  // What each presented frame actually cost, measured on the GL render thread
  // and split by stage (upload / compose / blit / present). Distinct from
  // video_output_fps above, which the player derives from media timestamps:
  // this one describes the rendering itself and survives a sink swap.
  media::RenderPerfStats render;

  StageTimings stages;
  base::TimeDelta first_frame_latency;
  size_t heap_bytes_estimate{0};
  uint64_t events_dropped_by_overflow{0};

  // Serialises the stats as JSON, for logs and monitoring pipelines.
  // Errors: none. Side effects: none.
  std::string ToJson() const;
};

}  // namespace avbase

#endif  // AVBASE_PLAYER_PUBLIC_PLAYBACK_STATS_H_
