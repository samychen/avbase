// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/vaapi/vaapi_video_decoder_factory.h"

#include "platform/ffmpeg/ffmpeg_hw_video_decoder.h"

namespace avbase::platform::vaapi {
namespace {

ffmpeg::FFmpegHwDecoderSpec MakeSpec() {
  ffmpeg::FFmpegHwDecoderSpec spec;
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

}  // namespace

VaapiVideoDecoderFactory::VaapiVideoDecoderFactory(
    base::scoped_refptr<base::SequencedTaskRunner> task_runner,
    media::HwCodecMask allowed_codecs)
    : task_runner_(std::move(task_runner)), allowed_codecs_(allowed_codecs) {}

media::VideoDecoderCapability VaapiVideoDecoderFactory::GetCapability() const {
  media::VideoDecoderCapability cap;
  cap.hardware = true;
  cap.outputs_opaque_surface = true;
  cap.handles_resolution_change = true;
  cap.max_width = 8192;
  cap.max_height = 8192;
  cap.priority = 10;
  return cap;
}

bool VaapiVideoDecoderFactory::SupportsCodec(
    media::VideoDecoderType type_hint) const {
  return type_hint == media::VideoDecoderType::kVaapiVideoDecoder;
}

std::unique_ptr<media::VideoDecoder>
VaapiVideoDecoderFactory::CreateVideoDecoder(
    const media::VideoDecoderConfig& config) {
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

}  // namespace avbase::platform::vaapi
