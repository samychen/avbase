// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_HWACCEL_VAAPI_HW_SPEC_H_
#define AVBASE_PLATFORM_HWACCEL_VAAPI_HW_SPEC_H_

#include "media/base/media_types.h"
#include "media/ffmpeg/ffmpeg_hw_video_decoder.h"

namespace avbase::platform::hwaccel {

// Linux VAAPI behind libavcodec's vaapi hwaccel. Frames leave as
// VASurfaceID-backed VideoFrames; EGL import happens at the display layer,
// pixels only on an explicit ToI420().
inline media::ffmpeg::FFmpegHwDecoderSpec VaapiHwSpec() {
  media::ffmpeg::FFmpegHwDecoderSpec spec;
  spec.device_type = "vaapi";
  // Empty device_name: FFmpeg opens the default render node. A multi-GPU
  // host that wants a specific node passes it via PlayerDeps once the
  // device-plumbing option exists.
  spec.handle_kind = media::NativeHandleKind::kVaapiSurface;
  spec.codecs = {media::VideoCodec::kH264, media::VideoCodec::kHevc,
                 media::VideoCodec::kVp9, media::VideoCodec::kAv1,
                 media::VideoCodec::kMpeg2Video};
  spec.decoder_type = media::VideoDecoderType::kVaapiVideoDecoder;
  spec.display_name = "VaapiDecoder";
  return spec;
}

}  // namespace avbase::platform::hwaccel

#endif  // AVBASE_PLATFORM_HWACCEL_VAAPI_HW_SPEC_H_
