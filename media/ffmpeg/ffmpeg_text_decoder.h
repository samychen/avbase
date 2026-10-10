// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_TEXT_DECODER_H_
#define AVBASE_MEDIA_FFMPEG_TEXT_DECODER_H_

#include <memory>
#include <string>
#include <vector>

#include "media/base/text_decoder.h"
#include "media/media_export.h"

namespace avbase::media {

// FFmpeg-backed subtitle decoder (subrip/srt, mov_text/tx3g, ass, webvtt --
// whatever the linked FFmpeg build enables). Synchronous push-through: one
// subtitle packet in, the cues it displays out. Packets must carry
// ff::AvPacketStorage, exactly like the audio/video decoders' input contract.
class AVBASE_MEDIA_EXPORT FFmpegTextDecoder final : public TextDecoder {
 public:
  FFmpegTextDecoder();
  FFmpegTextDecoder(const FFmpegTextDecoder&) = delete;
  FFmpegTextDecoder& operator=(const FFmpegTextDecoder&) = delete;
  ~FFmpegTextDecoder() override;

  const char* name() const override { return "FFmpegTextDecoder"; }
  Status Initialize(const TextDecoderConfig& config) override;
  Status Decode(const DecoderBuffer& buffer,
                std::vector<TimedTextCue>* cues) override;

 private:
  struct Context;
  std::unique_ptr<Context> ctx_;
  TextDecoderConfig config_;
};

// Factory wired by the FFmpeg layer (see text_decoder.h for why the factory
// indirection exists).
class AVBASE_MEDIA_EXPORT FFmpegTextDecoderFactory final
    : public TextDecoderFactory {
 public:
  std::unique_ptr<TextDecoder>
  CreateTextDecoder(const TextDecoderConfig& config) override;
  const char* name() const override { return "ffmpeg-text"; }

 private:
  friend class base::RefCountedThreadSafe<TextDecoderFactory>;
  ~FFmpegTextDecoderFactory() override = default;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FFMPEG_TEXT_DECODER_H_
