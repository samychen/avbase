// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_MEDIA_FILTERS_FFMPEG_DECODER_FACTORIES_H_
#define IJKPP_MEDIA_FILTERS_FFMPEG_DECODER_FACTORIES_H_

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/audio_decoder_factory.h"
#include "media/base/video_decoder_factory.h"
#include "media/media_export.h"

namespace ijkpp::media {

// Software-decoder factories backed by FFmpeg. They exist so that a platform
// can inject hardware factories *ahead* of them (RendererFactory::
// GetVideoDecoderFactory) without media/filters knowing about FFmpeg -- the
// factories themselves are the only place that must construct the concrete
// FFmpeg decoders.
//
// |task_runner| is where the decoder posts its callbacks; it must be the same
// sequence the owning DecoderStream runs on (S3 for video, S4 for audio),
// because DecoderStream's contract is that everything stays on one sequence.
class IJKPP_MEDIA_EXPORT FFmpegVideoDecoderFactory final
    : public VideoDecoderFactory {
 public:
  explicit FFmpegVideoDecoderFactory(
      base::scoped_refptr<base::SequencedTaskRunner> task_runner);
  FFmpegVideoDecoderFactory(const FFmpegVideoDecoderFactory&) = delete;
  FFmpegVideoDecoderFactory& operator=(const FFmpegVideoDecoderFactory&) =
      delete;

  VideoDecoderCapability GetCapability() const override;
  bool SupportsCodec(VideoDecoderType type_hint) const override;
  std::unique_ptr<VideoDecoder> CreateVideoDecoder(
      const VideoDecoderConfig& config) override;
  const char* name() const override { return "ffmpeg-video"; }

 private:
  friend class base::RefCountedThreadSafe<VideoDecoderFactory>;
  ~FFmpegVideoDecoderFactory() override = default;

  base::scoped_refptr<base::SequencedTaskRunner> task_runner_;
};

class IJKPP_MEDIA_EXPORT FFmpegAudioDecoderFactory final
    : public AudioDecoderFactory {
 public:
  explicit FFmpegAudioDecoderFactory(
      base::scoped_refptr<base::SequencedTaskRunner> task_runner);
  FFmpegAudioDecoderFactory(const FFmpegAudioDecoderFactory&) = delete;
  FFmpegAudioDecoderFactory& operator=(const FFmpegAudioDecoderFactory&) =
      delete;

  std::unique_ptr<AudioDecoder> CreateAudioDecoder(
      const AudioDecoderConfig& config) override;
  const char* name() const override { return "ffmpeg-audio"; }

 private:
  friend class base::RefCountedThreadSafe<AudioDecoderFactory>;
  ~FFmpegAudioDecoderFactory() override = default;

  base::scoped_refptr<base::SequencedTaskRunner> task_runner_;
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_FILTERS_FFMPEG_DECODER_FACTORIES_H_
