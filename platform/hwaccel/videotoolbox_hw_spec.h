// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_HWACCEL_VIDEOTOOLBOX_HW_SPEC_H_
#define AVBASE_PLATFORM_HWACCEL_VIDEOTOOLBOX_HW_SPEC_H_

#include <cstdlib>

#include "media/base/media_types.h"
#include "media/ffmpeg/ffmpeg_hw_video_decoder.h"

namespace avbase::platform::hwaccel {
namespace {

// VideoToolbox decodes H.264 at 4:2:0 only, and it says so at the worst
// possible moment: avcodec_open2() succeeds, and the first packet fails with
// "Failed setup for format videotoolbox_vld: hwaccel initialisation returned
// error" + "decode_slice_header error" (measured on macOS 15 / Apple silicon
// with a 3 s High 4:4:4 Predictive clip; a High 4:2:2 clip fails the same
// way, while High, High 10 and Constrained Baseline play). 20 consecutive
// failures later DecoderStream abandons the stream and the player reports
// kDecodeFailed -- for a file the software decoder plays fine.
//
// profile_idc 122 is the first 4:2:2 profile (High 4:2:2); 144 and 244 are
// the 4:4:4 ones. |profile| carries AVCodecParameters::profile as a decimal
// string (media/ffmpeg/ffmpeg_demuxer_configs.cc), so an empty or unparsable
// value means "not reported" -- give the hardware the benefit of the doubt
// there rather than refusing to play a stream it might handle.
constexpr int kH264ProfileHigh422 = 122;

bool VideotoolboxSupportsConfig(const media::VideoDecoderConfig& config) {
  if (config.codec != media::VideoCodec::kH264) {
    return true;  // HEVC/VP9: no measured chroma limit on this device.
  }
  if (config.profile.empty()) {
    return true;
  }
  const int idc = std::atoi(config.profile.c_str());
  return idc <= 0 || idc < kH264ProfileHigh422;
}

}  // namespace

// Apple VideoToolbox behind libavcodec's videotoolbox hwaccel. Frames leave
// the decoder as CVPixelBuffer-backed VideoFrames; pixels only on an explicit
// ToI420().
inline media::ffmpeg::FFmpegHwDecoderSpec VideotoolboxHwSpec() {
  media::ffmpeg::FFmpegHwDecoderSpec spec;
  spec.device_type = "videotoolbox";
  spec.handle_kind = media::NativeHandleKind::kCVPixelBuffer;
  // FFmpeg's videotoolbox hwaccel covers H.264, HEVC, VP9 and ProRes; ProRes
  // is not a media::VideoCodec yet, and advertising a codec the mask cannot
  // name would only produce a rejection later in the chain.
  spec.codecs = {media::VideoCodec::kH264, media::VideoCodec::kHevc,
                 media::VideoCodec::kVp9};
  spec.decoder_type = media::VideoDecoderType::kVideoToolbox;
  spec.display_name = "VideoToolboxDecoder";
  // Decline 4:2:2 / 4:4:4 H.264 here, before avcodec_open2() ever accepts the
  // stream, so the fallback chain still has the software decoder to walk to.
  // Once the first packet fails the chain has already moved on and only
  // kDecodeFailed is left (see the comment on VideotoolboxSupportsConfig).
  spec.config_supported = VideotoolboxSupportsConfig;
  return spec;
}

}  // namespace avbase::platform::hwaccel

#endif  // AVBASE_PLATFORM_HWACCEL_VIDEOTOOLBOX_HW_SPEC_H_
