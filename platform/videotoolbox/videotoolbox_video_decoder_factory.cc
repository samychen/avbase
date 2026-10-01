// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/videotoolbox/videotoolbox_video_decoder_factory.h"

#include "platform/ffmpeg/ffmpeg_hw_video_decoder.h"

namespace avbase::platform::videotoolbox {
namespace {

ffmpeg::FFmpegHwDecoderSpec MakeSpec() {
  ffmpeg::FFmpegHwDecoderSpec spec;
  spec.device_type = "videotoolbox";
  spec.handle_kind = media::NativeHandleKind::kCVPixelBuffer;
  // FFmpeg's videotoolbox hwaccel covers H.264, HEVC, VP9 and ProRes; ProRes
  // is not a media::VideoCodec yet, and advertising a codec the mask
  // cannot name
  // would only produce a rejection later in the chain.
  spec.codecs = {media::VideoCodec::kH264, media::VideoCodec::kHevc,
                 media::VideoCodec::kVp9};
  spec.decoder_type = media::VideoDecoderType::kVideoToolbox;
  spec.display_name = "VideoToolboxDecoder";
  return spec;
}

}  // namespace

VideoToolboxVideoDecoderFactory::VideoToolboxVideoDecoderFactory(
    base::scoped_refptr<base::SequencedTaskRunner> task_runner,
    media::HwCodecMask allowed_codecs)
    : task_runner_(std::move(task_runner)), allowed_codecs_(allowed_codecs) {}

media::VideoDecoderCapability
VideoToolboxVideoDecoderFactory::GetCapability() const {
  media::VideoDecoderCapability cap;
  cap.hardware = true;
  cap.outputs_opaque_surface = true;
  cap.handles_resolution_change = true;
  cap.max_width = 8192;
  cap.max_height = 8192;
  // Priority 10 keeps it above any other hardware candidate and strictly
  // above the software factory's 0.
  cap.priority = 10;
  return cap;
}

bool VideoToolboxVideoDecoderFactory::SupportsCodec(
    media::VideoDecoderType type_hint) const {
  return type_hint == media::VideoDecoderType::kVideoToolbox;
}

std::unique_ptr<media::VideoDecoder>
VideoToolboxVideoDecoderFactory::CreateVideoDecoder(
    const media::VideoDecoderConfig& config) {
  // Codec-level gate: hardware masks are the player-config's answer to
  // ijkplayer's mediacodec-avc/-hevc boolean soup; a codec the mask excludes
  // is a "not me" (the stream walks on to the software tail), not an error.
  if (!media::DecoderSelector::CodecAllowedByMask(config.codec,
                                                  allowed_codecs_)) {
    return nullptr;
  }
  bool codec_supported = false;
  for (const media::VideoCodec codec : MakeSpec().codecs) {
    if (codec == config.codec) {
      codec_supported = true;
      break;
    }
  }
  if (!codec_supported) {
    return nullptr;
  }
  return std::make_unique<ffmpeg::FFmpegHwVideoDecoder>(MakeSpec(),
                                                        task_runner_);
}

}  // namespace avbase::platform::videotoolbox
