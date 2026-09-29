// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/media_types.h` (BSD-3-Clause).

#ifndef IJKPP_MEDIA_BASE_MEDIA_TYPES_H_
#define IJKPP_MEDIA_BASE_MEDIA_TYPES_H_

#include "media/media_export.h"

namespace ijkpp::media {

// Mirrors media::DemuxerStream::Type, kept here so that value types can
// reference it without depending on the DemuxerStream interface.
enum class DemuxerStreamType {
  kUnknown = 0,
  kAudio = 1,
  kVideo = 2,
  kText = 3,
  kMaxValue = kText,
};

IJKPP_MEDIA_EXPORT const char* GetDemuxerStreamTypeName(DemuxerStreamType type);

enum class VideoDecoderType {
  kUnknown = 0,
  kFFmpegVideoDecoder,
  kVaapiVideoDecoder,
  kMediaCodec,
  kVideoToolbox,
  kNvDec,
  kMock,
};

enum class AudioDecoderType {
  kUnknown = 0,
  kFFmpegAudioDecoder,
  kMediaCodec,
  kAudioToolbox,
  kMock,
};

IJKPP_MEDIA_EXPORT const char* GetVideoDecoderTypeName(VideoDecoderType type);
IJKPP_MEDIA_EXPORT const char* GetAudioDecoderTypeName(AudioDecoderType type);

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_MEDIA_TYPES_H_
