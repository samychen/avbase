// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's media/base/video_decoder_config.h and
// audio_decoder_config.h (BSD-3-Clause), merged into one header because they
// share most of their shape and avbase does not need Chromium's per-field
// change tracking yet.

#ifndef AVBASE_MEDIA_BASE_DECODER_CONFIG_H_
#define AVBASE_MEDIA_BASE_DECODER_CONFIG_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/time/time.h"
#include "media/base/audio_parameters.h"
#include "media/base/media_types.h"
#include "media/base/video_frame.h"
#include "media/media_export.h"

namespace avbase::media {

// Fallback when the container reports no time base. Kept here rather than
// including libavutil (invariant C4).
inline constexpr int kAvTimeBase = 1000000;

// Codec identity as a stable string, matching FFmpeg's AVCodec::name so that
// logs and diagnostics are comparable across the two.
enum class VideoCodec {
  kUnknown = 0,
  kH264,
  kHevc,
  kVp8,
  kVp9,
  kAv1,
  kMpeg4,
  kMpeg2Video,
  kTheora,
  kMjpeg,
};
enum class AudioCodec {
  kUnknown = 0,
  kAac,
  kMp3,
  kOpus,
  kVorbis,
  kFlac,
  kPcmS16Le,
  kAc3,
  kEac3,
};

AVBASE_MEDIA_EXPORT const char* GetVideoCodecName(VideoCodec codec);
AVBASE_MEDIA_EXPORT const char* GetAudioCodecName(AudioCodec codec);
AVBASE_MEDIA_EXPORT VideoCodec VideoCodecFromName(std::string_view name);
AVBASE_MEDIA_EXPORT AudioCodec AudioCodecFromName(std::string_view name);

struct AVBASE_MEDIA_EXPORT VideoDecoderConfig {
  VideoCodec codec{VideoCodec::kUnknown};
  std::string codec_name;  // FFmpeg's name, e.g. "h264".
  std::string profile;
  std::string level;
  Size coded_size;
  Size natural_size;
  Rational sar{1, 1};
  int rotation{0};
  VideoFormat expected_output_format{VideoFormat::kUnknown};
  std::vector<uint8_t> extra_data;  // AVCodecParameters::extradata (SPS/PPS).
  Rational frame_rate{0, 1};
  Rational avg_frame_rate{0, 1};
  // The container stream's time base (AVStream::time_base). The decoder needs
  // it to interpret AVFrame::pts; without it every decoded timestamp is 0.
  Rational time_base{1, kAvTimeBase};
  int64_t bit_rate{0};
  bool has_hdr_metadata{false};

  bool IsValidConfig() const {
    return codec != VideoCodec::kUnknown && !coded_size.IsEmpty();
  }
};

// Subtitle/text track description (Phase 4.2: the base carries text + time,
// the host renders). FFmpeg text decoders initialise from the codec name and
// whatever private data the container attached (tx3g default track header,
// ASS style header).
struct AVBASE_MEDIA_EXPORT TextDecoderConfig {
  std::string codec_name;  // "subrip"/"srt", "mov_text", "ass", "webvtt".
  std::string language;    // ISO-639-2, empty when unknown.
  std::vector<uint8_t> extra_data;

  bool IsValidConfig() const { return !codec_name.empty(); }

  constexpr friend bool operator==(const TextDecoderConfig&,
                                   const TextDecoderConfig&) = default;
};

struct AVBASE_MEDIA_EXPORT AudioDecoderConfig {
  AudioCodec codec{AudioCodec::kUnknown};
  std::string codec_name;
  std::string profile;
  ChannelLayout channel_layout{ChannelLayout::kNone};
  SampleFormat sample_format{SampleFormat::kUnknown};
  int sample_rate{0};
  int channels{0};
  int64_t bit_rate{0};
  std::vector<uint8_t> extra_data;
  // AAC/ADTS needs this to size the priming padding correctly.
  int codec_delay_frames{0};
  int seek_preroll_frames{0};

  bool IsValidConfig() const {
    return codec != AudioCodec::kUnknown && sample_rate > 0 && channels > 0;
  }

  // True when the container gave enough to build renderable parameters.
  // Deliberately WEAKER than IsValidConfig(): it does not require a resolvable
  // codec, because "this build has no decoder for AC-3" is a decoder-layer
  // fact, whereas 0 channels is the container admitting it could not measure
  // the stream at all -- libavformat logs "Could not find codec parameters"
  // for exactly that case, and an MPEG-PS file can carry such a stream beside
  // a perfectly good one. Such a stream is not playable, and the difference
  // matters: the demuxer drops those, while it must keep reporting the ones it
  // merely cannot decode here.
  bool HasUsableParameters() const { return sample_rate > 0 && channels > 0; }
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_DECODER_CONFIG_H_
