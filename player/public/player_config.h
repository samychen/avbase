// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Every field names the ijkplayer option it replaces, so an existing
// setOption() call site can be migrated mechanically. Full table: docs/05 §4.

#ifndef AVBASE_PLAYER_PUBLIC_PLAYER_CONFIG_H_
#define AVBASE_PLAYER_PUBLIC_PLAYER_CONFIG_H_

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "media/base/video_frame.h"
#include "media/filters/decoder_selector.h"   // DecoderPreference, HwCodecMask
#include "player/public/player_export.h"

namespace avbase {

enum class SyncMasterType { kAudio = 0, kVideo, kExternal };
// DecoderPreference and HwCodecMask are defined in media/ (see invariant C22:
// media/ must not depend on player/, so the enum lives there and is re-exported).
using media::DecoderPreference;
using media::HwCodecFlag;
using media::HwCodecMask;
enum class OverlayFormat {
  kRgb32 = 0, kRgb24, kRgb16, kI420, kYv12, kNative,
};
enum class SeekMode { kPreviousKeyframe = 0, kAnyFrame, kAccurate };
enum class AudioBackend { kAuto = 0, kAlsa, kPulse, kPipeWire, kSdl2, kOpenSLES, kAAudio, kCoreAudio };
enum class LinuxVideoBackend { kAuto = 0, kSdl2, kGl };
enum class HdrToneMapping { kNone = 0, kSimple, kAuto };

struct AVBASE_PLAYER_EXPORT DemuxConfig {
  int64_t probe_size{5 * 1024 * 1024};                       // "probesize"
  base::TimeDelta analyze_duration{base::Seconds(5)};        // "analyzeduration"
  bool find_stream_info{true};                               // "find_stream_info"
  std::string forced_format;                                 // "iformat"
  base::TimeDelta timeout;                                   // "timeout"/"rw_timeout"
  bool delay_meta_init{false};                               // "ijkmeta-delay-init"

  friend bool operator==(const DemuxConfig&, const DemuxConfig&) = default;
};

struct AVBASE_PLAYER_EXPORT BufferConfig {
  bool enabled{true};                                        // "packet-buffering"
  size_t max_bytes{15 * 1024 * 1024};                        // "max-buffer-size"
  size_t min_frames{5};                                      // "min-frames"
  bool unlimited{false};                                     // "infbuf"
  base::TimeDelta max_cached_duration{base::Seconds(3)};
  // Three-tier high water mark: each stall->recovery cycle steps to the next,
  // and a seek resets to the first. Ported unchanged from ijkplayer.
  base::TimeDelta first_high_water_mark{base::Milliseconds(100)};
  base::TimeDelta next_high_water_mark{base::Seconds(1)};
  base::TimeDelta last_high_water_mark{base::Seconds(5)};
  bool sync_av_start{true};                                  // "sync-av-start"

  friend bool operator==(const BufferConfig&, const BufferConfig&) = default;
};

struct AVBASE_PLAYER_EXPORT SeekConfig {
  bool accurate{false};                                      // "enable-accurate-seek"
  base::TimeDelta accurate_timeout{base::Seconds(5)};        // "accurate-seek-timeout"
  base::TimeDelta seek_at_start;                             // "seek-at-start"
  bool flush_on_seek{true};

  friend bool operator==(const SeekConfig&, const SeekConfig&) = default;
};

struct AVBASE_PLAYER_EXPORT VideoConfig {
  bool disabled{false};                                      // "vn"
  DecoderPreference decoder_preference{DecoderPreference::kAuto};
  // Which codecs the hardware path may handle; each bit is an HwCodecFlag.
  // kAll (not 0) so that the Phase 3 default is "hardware first, fall back to
  // software", matching decoder_preference{kAuto}; opt OUT per codec with
  // `hw_codecs & ~HwCodecFlag::kAvc`, or force software via kSoftware.
  // Replaces ijkplayer's "mediacodec-*" booleans.
  HwCodecMask hw_codecs{static_cast<HwCodecMask>(HwCodecFlag::kAll)};
  std::string hw_decoder_name;                               // "mediacodec-default-name"
  bool hw_sync_mode{false};                                  // "mediacodec-sync"
  bool hw_async{false};                                      // "videotoolbox-async"
  bool hw_wait_async{true};                                  // "videotoolbox-wait-async"
  int hw_max_frame_width{0};                                 // "videotoolbox-max-frame-width"
  bool auto_rotate{false};                                   // "mediacodec-auto-rotate"
  // Defaults to true (behaviour difference Δ3): leaving resolution changes
  // unhandled corrupts the picture rather than degrading gracefully.
  bool handle_resolution_change{true};
  int max_fps{31};                                           // "max-fps"
  int max_frame_drop{0};                                     // "framedrop"
  OverlayFormat overlay_format{OverlayFormat::kRgb32};       // "overlay-format"
  int frame_queue_size{3};                                   // "video-pictq-size"
  int skip_loop_filter{0};                                   // "skip-loop-filter"
  bool calc_frame_rate{true};                                // !"skip-calc-frame-rate"
  std::string filter_graph;                                  // "vf0"
  int selected_stream{-1};                                   // "vst"
  std::string forced_mime_type;                              // "video-mime-type"
  HdrToneMapping hdr_tone_mapping{HdrToneMapping::kSimple};

  friend bool operator==(const VideoConfig&, const VideoConfig&) = default;
};

struct AVBASE_PLAYER_EXPORT AudioConfig {
  bool disabled{false};                                      // "an"
  double startup_volume{1.0};                                // "volume" (0..100 -> 0..1, Δ6)
  AudioBackend backend{AudioBackend::kAuto};                 // "opensles" etc.
  // True by default: uses the built-in WSOLA AudioRendererAlgorithm instead of
  // ijkplayer's SoundTouch dependency (behaviour difference Δ17).
  bool tempo_stretch{true};                                  // "soundtouch"
  bool preserves_pitch{true};
  int frame_queue_size{9};
  int selected_stream{-1};                                   // "ast"
  double preset_5_1_center_mix_level{0.70710678};            // "preset-5-1-center-mix-level"
  std::string filter_graph;                                  // "af"
  std::string output_device_id;

  friend bool operator==(const AudioConfig&, const AudioConfig&) = default;
};

struct AVBASE_PLAYER_EXPORT SubtitleConfig {
  bool enabled{false};                                       // "subtitle"
  int selected_stream{-1};                                   // "sst"

  // How far behind the media clock a cue may be and still be shown, on a LIVE
  // source. Zero (the default) disables the drop, which is what recorded
  // content wants: a seek lands on a subtitle, and that cue is exactly what the
  // viewer asked for.
  //
  // On a live edge it is the other way round. The demuxer keeps delivering
  // from the point the viewer joined, so a viewer who joins late, or whose
  // pipeline stalls, receives cues for dialogue that has already been spoken
  // and shown. Displaying those is worse than displaying none -- the text is
  // right, the timing is nonsense -- so a cue further behind than this window
  // is dropped instead.
  //
  // The default is deliberately generous (10 s): a window that is too narrow
  // eats subtitles during a transient stall, which is a worse failure than a
  // late line, so this should be set to the longest stall the product is
  // willing to paper over, not to a "typical" one.
  base::TimeDelta live_cue_max_age{base::Seconds(10)};

  friend bool operator==(const SubtitleConfig&, const SubtitleConfig&) =
      default;
};

struct AVBASE_PLAYER_EXPORT NetConfig {
  bool reconnect{true};
  int reconnect_max_retries{3};
  base::TimeDelta reconnect_delay{base::Milliseconds(100)};
  bool dns_cache_clear{false};                               // "dns_cache_clear"
  std::string user_agent;
  std::map<std::string, std::string> headers;
  // Zero means "do not chase the live edge".
  base::TimeDelta live_max_latency;

  friend bool operator==(const NetConfig&, const NetConfig&) = default;
};

struct AVBASE_PLAYER_EXPORT DataSourceConfig {
  bool enable_cache{false};                                  // ijkio master switch (M18)
  std::string cache_dir;
  size_t cache_max_bytes{512 * 1024 * 1024};

  friend bool operator==(const DataSourceConfig&, const DataSourceConfig&) =
      default;
};

struct AVBASE_PLAYER_EXPORT RenderConfig {
  bool disable_video_output{false};                          // "nodisp"
  bool render_wait_start{false};                             // "render-wait-start"
  LinuxVideoBackend linux_backend{LinuxVideoBackend::kAuto};
  AudioBackend linux_audio{AudioBackend::kAuto};
  bool prefer_dmabuf_zero_copy{true};
  bool use_present_extension_for_vsync{true};
  bool gl_context_sharing{false};
  bool prefer_xshm{false};

  friend bool operator==(const RenderConfig&, const RenderConfig&) = default;
};

struct AVBASE_PLAYER_EXPORT PlayerConfig {
  DemuxConfig demux;
  BufferConfig buffer;
  SeekConfig seek;
  VideoConfig video;
  AudioConfig audio;
  SubtitleConfig subtitle;
  NetConfig net;
  DataSourceConfig data_source;
  RenderConfig render;

  SyncMasterType sync_master{SyncMasterType::kAudio};        // "sync"
  int loop_count{1};                                         // "loop" (-1 = infinite)
  bool fast{false};                                          // "fast"
  bool start_on_prepared{true};                              // "start-on-prepared"
  bool async_init_decoder{false};                            // "async-init-decoder"
  bool no_time_adjust{false};                                // "no-time-adjust"
  bool get_frame_mode{false};                                // "get-frame-mode"
  int rdft_speed{0};                                         // "rdftspeed"
  // Upper bound on ~Player() and StopSync(). On expiry the offending sequence
  // is detached and logged rather than blocking the caller forever (Δ15).
  base::TimeDelta shutdown_timeout{base::Milliseconds(500)};
  base::TimeDelta stats_interval{base::Seconds(1)};

  // Escape hatches: passed through verbatim to FFmpeg, matching ijkplayer's
  // IJKMP_OPT_CATEGORY_FORMAT / CODEC / SWS.
  std::map<std::string, std::string> extra_format_options;
  std::map<std::string, std::string> extra_codec_options;
  std::map<std::string, std::string> extra_scaler_options;

  // Each nested config states its own defaulted comparison; without them this
  // one is implicitly deleted (clang: -Wdefaulted-function-deleted), so
  // "compare two configs" was a compile error waiting for its first user.
  friend bool operator==(const PlayerConfig&, const PlayerConfig&) = default;
};

struct AVBASE_PLAYER_EXPORT ConfigIssue {
  std::string field;        // "buffer.first_high_water_mark"
  std::string problem;      // "must be <= buffer.next_high_water_mark"
  std::string suggestion;
};

// Returns every problem at once, so a caller can fix them in a single pass
// instead of discovering them one rebuild at a time.
AVBASE_PLAYER_EXPORT std::vector<ConfigIssue> ValidateConfig(const PlayerConfig& config);

}  // namespace avbase

#endif  // AVBASE_PLAYER_PUBLIC_PLAYER_CONFIG_H_
