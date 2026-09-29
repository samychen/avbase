// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Lives in media/base: media::Demuxer produces it. player/public/media_info.h
// re-exports it (invariant C22).

#ifndef IJKPP_MEDIA_BASE_MEDIA_INFO_H_
#define IJKPP_MEDIA_BASE_MEDIA_INFO_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "media/base/media_types.h"
#include "media/base/video_frame.h"
#include "media/media_export.h"

namespace ijkpp::media {

enum class StreamKind { kUnknown, kAudio, kVideo, kText };

struct IJKPP_MEDIA_EXPORT StreamInfo {
  int index{-1};
  StreamKind kind{StreamKind::kUnknown};
  std::string codec_name;        // "h264", "hevc", "aac", "opus", "subrip"...
  std::string language;          // ISO-639-2, empty when unknown.
  std::string title;
  base::TimeDelta duration;

  // Video only.
  media::Size coded_size;
  media::Size natural_size;      // After SAR is applied.
  media::Rational sar{1, 1};
  media::Rational frame_rate{0, 1};
  media::Rational avg_frame_rate{0, 1};
  int rotation{0};
  std::string pixel_format;
  bool has_hdr_metadata{false};

  // Audio only.
  int sample_rate{0};
  int channels{0};
  std::string channel_layout;
  std::string sample_format;

  int64_t bit_rate{0};
  bool is_default{false};
  bool is_forced{false};
  std::map<std::string, std::string> metadata;
};

// Everything the container told us, as one immutable snapshot. Replaces
// ijkplayer's ijkmeta.c plus a dozen ijkmp_get_* accessors.
struct IJKPP_MEDIA_EXPORT MediaInfo {
  base::TimeDelta duration;
  // Live streams report a zero duration; use |is_live| rather than testing it.
  bool is_live{false};
  bool seekable{true};
  int64_t bit_rate{0};
  int64_t file_size{-1};
  std::string format_name;       // "mov,mp4,m4a,3gp,3g2,mj2"
  std::string uri;
  base::TimeDelta start_time;
  std::vector<StreamInfo> streams;
  std::map<std::string, std::string> metadata;

  // -1 when there is no such stream.
  int FirstStreamOfKind(StreamKind kind) const;
  const StreamInfo* FindStream(int index) const;
  std::vector<const StreamInfo*> StreamsOfKind(StreamKind kind) const;
  // Total duration may be an estimate for streaming containers.
  bool duration_is_estimate{false};
};


}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_MEDIA_INFO_H_
