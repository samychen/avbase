// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_hw_decoder_factory.h"

#include <memory>
#include <utility>

namespace avbase::media::ffmpeg {

FFmpegHwVideoDecoderFactory::FFmpegHwVideoDecoderFactory(
    FFmpegHwDecoderSpec spec,
    base::scoped_refptr<base::SequencedTaskRunner> task_runner,
    media::HwCodecMask allowed_codecs)
    : spec_(std::move(spec)),
      task_runner_(std::move(task_runner)),
      allowed_codecs_(allowed_codecs) {}

media::VideoDecoderCapability
FFmpegHwVideoDecoderFactory::GetCapability() const {
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

bool FFmpegHwVideoDecoderFactory::SupportsCodec(
    media::VideoDecoderType type_hint) const {
  return type_hint == spec_.decoder_type;
}

std::unique_ptr<media::VideoDecoder>
FFmpegHwVideoDecoderFactory::CreateVideoDecoder(
    const media::VideoDecoderConfig& config) {
  // Codec-level gate: hardware masks are the player-config's answer to
  // ijkplayer's mediacodec-avc/-hevc boolean soup; a codec the mask excludes
  // is a "not me" (the stream walks on to the software tail), not an error.
  if (!media::DecoderSelector::CodecAllowedByMask(config.codec,
                                                  allowed_codecs_)) {
    return nullptr;
  }
  for (const media::VideoCodec codec : spec_.codecs) {
    if (codec == config.codec) {
      return std::make_unique<FFmpegHwVideoDecoder>(spec_, task_runner_);
    }
  }
  return nullptr;
}

}  // namespace avbase::media::ffmpeg
