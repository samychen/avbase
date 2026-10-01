// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/media_types.h"

namespace avbase::media {

const char* GetDemuxerStreamTypeName(DemuxerStreamType type) {
  switch (type) {
    case DemuxerStreamType::kUnknown: return "unknown";
    case DemuxerStreamType::kAudio:   return "audio";
    case DemuxerStreamType::kVideo:   return "video";
    case DemuxerStreamType::kText:
      return "text";
    // kMaxValue aliases kText, so it has no case of its own.
  }
  return "invalid";
}

const char* GetVideoDecoderTypeName(VideoDecoderType type) {
  switch (type) {
    case VideoDecoderType::kUnknown:             return "unknown";
    case VideoDecoderType::kFFmpegVideoDecoder:  return "FFmpegVideoDecoder";
    case VideoDecoderType::kVaapiVideoDecoder:   return "VaapiVideoDecoder";
    case VideoDecoderType::kMediaCodec:          return "MediaCodec";
    case VideoDecoderType::kVideoToolbox:        return "VideoToolbox";
    case VideoDecoderType::kNvDec:               return "NvDec";
    case VideoDecoderType::kD3D11VideoDecoder:   return "D3D11VideoDecoder";
    case VideoDecoderType::kMock:                return "Mock";
  }
  return "invalid";
}

const char* GetAudioDecoderTypeName(AudioDecoderType type) {
  switch (type) {
    case AudioDecoderType::kUnknown:            return "unknown";
    case AudioDecoderType::kFFmpegAudioDecoder: return "FFmpegAudioDecoder";
    case AudioDecoderType::kMediaCodec:         return "MediaCodec";
    case AudioDecoderType::kAudioToolbox:       return "AudioToolbox";
    case AudioDecoderType::kMock:               return "Mock";
  }
  return "invalid";
}

}  // namespace avbase::media
