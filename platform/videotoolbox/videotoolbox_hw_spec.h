// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_VIDEOTOOLBOX_VIDEOTOOLBOX_HW_SPEC_H_
#define AVBASE_PLATFORM_VIDEOTOOLBOX_VIDEOTOOLBOX_HW_SPEC_H_

#include "media/base/media_types.h"
#include "platform/ffmpeg/ffmpeg_hw_video_decoder.h"

namespace avbase::platform::videotoolbox {

// Apple VideoToolbox behind libavcodec's videotoolbox hwaccel. Frames leave
// the decoder as CVPixelBuffer-backed VideoFrames; pixels only on an explicit
// ToI420().
inline ffmpeg::FFmpegHwDecoderSpec VideotoolboxHwSpec() {
  ffmpeg::FFmpegHwDecoderSpec spec;
  spec.device_type = "videotoolbox";
  spec.handle_kind = media::NativeHandleKind::kCVPixelBuffer;
  // FFmpeg's videotoolbox hwaccel covers H.264, HEVC, VP9 and ProRes; ProRes
  // is not a media::VideoCodec yet, and advertising a codec the mask cannot
  // name would only produce a rejection later in the chain.
  spec.codecs = {media::VideoCodec::kH264, media::VideoCodec::kHevc,
                 media::VideoCodec::kVp9};
  spec.decoder_type = media::VideoDecoderType::kVideoToolbox;
  spec.display_name = "VideoToolboxDecoder";
  return spec;
}

}  // namespace avbase::platform::videotoolbox

#endif  // AVBASE_PLATFORM_VIDEOTOOLBOX_VIDEOTOOLBOX_HW_SPEC_H_
