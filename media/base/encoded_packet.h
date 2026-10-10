// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_BASE_ENCODED_PACKET_H_
#define AVBASE_MEDIA_BASE_ENCODED_PACKET_H_

#include <cstdint>
#include <vector>

#include "media/media_export.h"

namespace avbase::media {

// The encoder output: one compressed packet plus the metadata the muxer needs
// to write it correctly. ffmpeg.c threads AVPacket end-to-end through encode →
// mux; avbase splits the encoder and muxer into separate objects, so this
// struct carries the AVPacket fields that cross that boundary.
//
// Fields mirror AVPacket:
//   data      — the compressed payload.
//   pts/dts   — in the encoder's timebase (audio: 1/sample_rate, video:
//               1/framerate_denominator). The muxer rescales on write.
//   duration  — in the same timebase; 0 if unknown.
//   flags     — AV_PKT_FLAG_KEY etc.  Bit 0 = keyframe. The muxer passes
//               these through verbatim instead of guessing.
//   side_data — extradata-like attachments the encoder emits per-packet
//               (e.g. h264 SPS/PPS in-band). The muxer copies these into
//               the AVPacket it builds for av_interleaved_write_frame.

// AV_NOPTS_VALUE is INT64_MIN in FFmpeg; aliased here so this header stays
// free of FFmpeg includes (design goal G2: nothing in this layer may name a
// vendor symbol).
//
// WHY THIS FILE IS IN media/base AND NOT NEXT TO ITS CONSUMERS. It is the
// encode-side mirror of media/base/decoder_buffer.h -- the same "one
// compressed payload plus its timestamps" shape on the way out rather than
// on the way in -- and it has zero dependencies of its own: <cstdint>,
// <vector>, and nothing else. Its consumers are all in media/transcode,
// which is a separate target built on FFmpeg; leaving the value type there
// would have made the boundary between "what crosses the encoder/muxer seam"
// and "who touches libav*" the same line, which is exactly the confusion
// docs/02 §4.1 exists to prevent.
constexpr int64_t kNoPtsValue = INT64_MIN;
struct AVBASE_MEDIA_EXPORT EncodedPacket {
  std::vector<uint8_t> data;
  int64_t pts = kNoPtsValue;
  int64_t dts = kNoPtsValue;
  int64_t duration = 0;
  int flags = 0;

  // Side data entries (type + bytes), mirroring AVPacketSideData.
  struct SideData {
    int type = 0;  // AV_PKT_DATA_* constant.
    std::vector<uint8_t> bytes;
  };
  std::vector<SideData> side_data;

  // AV_PKT_FLAG_KEY is bit 0 in FFmpeg's flag set. Exposed as a helper so
  // callers and tests do not need to hardcode the constant.
  static constexpr int kKeyFrameFlag = 0x0001;

  bool is_key_frame() const { return (flags & kKeyFrameFlag) != 0; }

  // Pack/unpack helpers for the legacy vector<vector<uint8_t>> API.
  static std::vector<std::vector<uint8_t>>
  StripToBytes(const std::vector<EncodedPacket>& packets) {
    std::vector<std::vector<uint8_t>> out;
    out.reserve(packets.size());
    for (const auto& p : packets) {
      out.push_back(p.data);
    }
    return out;
  }
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_ENCODED_PACKET_H_
