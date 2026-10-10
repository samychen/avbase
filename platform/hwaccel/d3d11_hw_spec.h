// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_HWACCEL_D3D11_HW_SPEC_H_
#define AVBASE_PLATFORM_HWACCEL_D3D11_HW_SPEC_H_

#include "media/base/media_types.h"
#include "platform/ffmpeg/ffmpeg_hw_video_decoder.h"

namespace avbase::platform::hwaccel {

// Windows D3D11VA behind libavcodec's d3d11va hwaccel (NVDEC and QuickSync
// both surface through it). Frames leave as ID3D11Texture2D-backed
// VideoFrames with the subresource index in the typed NativeHandle.
inline ffmpeg::FFmpegHwDecoderSpec D3D11HwSpec() {
  ffmpeg::FFmpegHwDecoderSpec spec;
  spec.device_type = "d3d11va";
  spec.handle_kind = media::NativeHandleKind::kD3D11Texture;
  spec.codecs = {media::VideoCodec::kH264, media::VideoCodec::kHevc,
                 media::VideoCodec::kVp9, media::VideoCodec::kAv1};
  spec.decoder_type = media::VideoDecoderType::kD3D11VideoDecoder;
  spec.display_name = "D3D11Decoder";
  return spec;
}

}  // namespace avbase::platform::hwaccel

#endif  // AVBASE_PLATFORM_HWACCEL_D3D11_HW_SPEC_H_
