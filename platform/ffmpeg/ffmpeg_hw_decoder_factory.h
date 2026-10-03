// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_FFMPEG_FFMPEG_HW_DECODER_FACTORY_H_
#define AVBASE_PLATFORM_FFMPEG_FFMPEG_HW_DECODER_FACTORY_H_

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/video_decoder_factory.h"
#include "media/filters/decoder_selector.h"
#include "platform/ffmpeg/ffmpeg_hw_video_decoder.h"

namespace avbase::platform::ffmpeg {

// ONE hardware factory, parameterized by FFmpegHwDecoderSpec. The three
// platform backends (videotoolbox / vaapi / d3d11) differ only in the spec --
// device name, NativeHandle kind, codec list -- which each platform header
// (platform/<name>/<name>_hw_spec.h) provides. This replaced three
// copy-pasted factory classes whose diff was pure renaming.
//
// Still FFmpeg-hwaccel mediated: the decode work is the platform's video
// device behind libavcodec, and invariant C8 keeps the libav includes inside
// this target. Frames leave as typed NativeHandles; pixels only on an
// explicit ToI420().
class FFmpegHwVideoDecoderFactory final : public media::VideoDecoderFactory {
 public:
  FFmpegHwVideoDecoderFactory(
      FFmpegHwDecoderSpec spec,
      base::scoped_refptr<base::SequencedTaskRunner> task_runner,
      media::HwCodecMask allowed_codecs);
  FFmpegHwVideoDecoderFactory(const FFmpegHwVideoDecoderFactory&) = delete;
  FFmpegHwVideoDecoderFactory&
  operator=(const FFmpegHwVideoDecoderFactory&) = delete;

  media::VideoDecoderCapability GetCapability() const override;
  bool SupportsCodec(media::VideoDecoderType type_hint) const override;
  std::unique_ptr<media::VideoDecoder>
  CreateVideoDecoder(const media::VideoDecoderConfig& config) override;
  const char* name() const override { return spec_.display_name.c_str(); }

 private:
  friend class base::RefCountedThreadSafe<VideoDecoderFactory>;
  ~FFmpegHwVideoDecoderFactory() override = default;

  const FFmpegHwDecoderSpec spec_;
  base::scoped_refptr<base::SequencedTaskRunner> task_runner_;
  media::HwCodecMask allowed_codecs_;
};

}  // namespace avbase::platform::ffmpeg

#endif  // AVBASE_PLATFORM_FFMPEG_FFMPEG_HW_DECODER_FACTORY_H_
