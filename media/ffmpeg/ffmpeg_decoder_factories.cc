// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_decoder_factories.h"

#include <memory>
#include <utility>

#include "media/ffmpeg/ffmpeg_audio_decoder.h"
#include "media/ffmpeg/ffmpeg_video_decoder.h"

namespace avbase::media {

FFmpegVideoDecoderFactory::FFmpegVideoDecoderFactory(
    base::scoped_refptr<base::SequencedTaskRunner> task_runner)
    : task_runner_(std::move(task_runner)) {}

VideoDecoderCapability FFmpegVideoDecoderFactory::GetCapability() const {
  // Software decode: no hardware bits, no surface output, no resolution
  // ceiling. Priority 0 keeps it below every hardware candidate, which is the
  // Auto preference's whole point (Δ12's fallback lands here when the
  // hardware decoders fail to open).
  return VideoDecoderCapability{};
}

bool FFmpegVideoDecoderFactory::SupportsCodec(
    VideoDecoderType type_hint) const {
  // FFmpeg handles every codec the container can name, including the ones the
  // hardware masks do not cover (kUnknown, kTheora, ...). Returning true
  // unconditionally is what makes it a working last resort rather than a
  // codec-filtered candidate.
  (void)type_hint;
  return true;
}

std::unique_ptr<VideoDecoder> FFmpegVideoDecoderFactory::CreateVideoDecoder(
    const VideoDecoderConfig& config) {
  // The factory does not filter configs: DecoderSelector already ranked the
  // factories, and the decoder itself reports kUnsupportedCodec/kUnsupported
  // Resolution for a config it cannot open (M5's actionable-error split).
  (void)config;
  return std::make_unique<FFmpegVideoDecoder>(task_runner_);
}

FFmpegAudioDecoderFactory::FFmpegAudioDecoderFactory(
    base::scoped_refptr<base::SequencedTaskRunner> task_runner)
    : task_runner_(std::move(task_runner)) {}

std::unique_ptr<AudioDecoder> FFmpegAudioDecoderFactory::CreateAudioDecoder(
    const AudioDecoderConfig& config) {
  (void)config;
  return std::make_unique<FFmpegAudioDecoder>();
}

}  // namespace avbase::media
