// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_FFMPEG_HW_DECODER_FACTORY_H_
#define AVBASE_MEDIA_FFMPEG_FFMPEG_HW_DECODER_FACTORY_H_

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/video_decoder_factory.h"
#include "media/ffmpeg/ffmpeg_hw_video_decoder.h"
#include "media/filters/decoder_selector.h"

namespace avbase::media::ffmpeg {

// ONE hardware factory, parameterized by FFmpegHwDecoderSpec. The three
// platform backends (videotoolbox / vaapi / d3d11) differ only in the spec --
// device name, NativeHandle kind, codec list -- which each backend header
// (platform/hwaccel/<name>_hw_spec.h) provides. This replaced three
// copy-pasted factory classes whose diff was pure renaming.
//
// The three specs share ONE directory rather than getting one each: a spec is
// a single inline function, and three directories holding one 30-line header
// apiece read as three backend modules that do not exist. A backend that grows
// real code -- not just a spec -- gets promoted back to platform/<name>/.
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

}  // namespace avbase::media::ffmpeg

#endif  // AVBASE_MEDIA_FFMPEG_FFMPEG_HW_DECODER_FACTORY_H_
