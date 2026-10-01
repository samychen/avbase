// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/media_types.h` (BSD-3-Clause).

#ifndef AVBASE_MEDIA_BASE_MEDIA_TYPES_H_
#define AVBASE_MEDIA_BASE_MEDIA_TYPES_H_

#include "media/media_export.h"

namespace avbase::media {

// Mirrors media::DemuxerStream::Type, kept here so that value types can
// reference it without depending on the DemuxerStream interface.
enum class DemuxerStreamType {
  kUnknown = 0,
  kAudio = 1,
  kVideo = 2,
  kText = 3,
  kMaxValue = kText,
};

AVBASE_MEDIA_EXPORT const char* GetDemuxerStreamTypeName(
    DemuxerStreamType type);

enum class VideoDecoderType {
  kUnknown = 0,
  kFFmpegVideoDecoder,
  kVaapiVideoDecoder,
  kMediaCodec,
  kVideoToolbox,
  kNvDec,
  kD3D11VideoDecoder,   // D3D11VA (NVDEC/QuickSync surface through it too).
  kMock,
};

enum class AudioDecoderType {
  kUnknown = 0,
  kFFmpegAudioDecoder,
  kMediaCodec,
  kAudioToolbox,
  kMock,
};

AVBASE_MEDIA_EXPORT const char* GetVideoDecoderTypeName(VideoDecoderType type);
AVBASE_MEDIA_EXPORT const char* GetAudioDecoderTypeName(AudioDecoderType type);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_MEDIA_TYPES_H_
