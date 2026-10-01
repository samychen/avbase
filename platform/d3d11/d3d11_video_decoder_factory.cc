// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/d3d11/d3d11_video_decoder_factory.h"

#include "platform/ffmpeg/ffmpeg_hw_video_decoder.h"

namespace avbase::platform::d3d11 {
namespace {

ffmpeg::FFmpegHwDecoderSpec MakeSpec() {
  ffmpeg::FFmpegHwDecoderSpec spec;
  spec.device_type = "d3d11va";
  spec.handle_kind = media::NativeHandleKind::kD3D11Texture;
  spec.codecs = {media::VideoCodec::kH264, media::VideoCodec::kHevc,
                 media::VideoCodec::kVp9, media::VideoCodec::kAv1};
  spec.decoder_type = media::VideoDecoderType::kD3D11VideoDecoder;
  spec.display_name = "D3D11Decoder";
  return spec;
}

}  // namespace

D3D11VideoDecoderFactory::D3D11VideoDecoderFactory(
    base::scoped_refptr<base::SequencedTaskRunner> task_runner,
    media::HwCodecMask allowed_codecs)
    : task_runner_(std::move(task_runner)), allowed_codecs_(allowed_codecs) {}

media::VideoDecoderCapability D3D11VideoDecoderFactory::GetCapability() const {
  media::VideoDecoderCapability cap;
  cap.hardware = true;
  cap.outputs_opaque_surface = true;
  cap.handles_resolution_change = true;
  cap.max_width = 8192;
  cap.max_height = 8192;
  cap.priority = 10;
  return cap;
}

bool D3D11VideoDecoderFactory::SupportsCodec(
    media::VideoDecoderType type_hint) const {
  return type_hint == media::VideoDecoderType::kD3D11VideoDecoder;
}

std::unique_ptr<media::VideoDecoder>
D3D11VideoDecoderFactory::CreateVideoDecoder(
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

}  // namespace avbase::platform::d3d11
