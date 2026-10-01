// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_VIDEOTOOLBOX_VIDEOTOOLBOX_VIDEO_DECODER_FACTORY_H_
#define AVBASE_PLATFORM_VIDEOTOOLBOX_VIDEOTOOLBOX_VIDEO_DECODER_FACTORY_H_

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/video_decoder_factory.h"
#include "media/filters/decoder_selector.h"

namespace avbase::platform::videotoolbox {

// Apple VideoToolbox hardware decoder factory (FFmpeg-hwaccel mediated; the
// decode work is done by VTDecompressionSession behind libavcodec's
// videotoolbox hwaccel). Frames leave the decoder as zero-copy
// CVPixelBuffer-backed VideoFrames (NativeHandleKind::kCVPixelBuffer); no
// pixel is read back unless a consumer explicitly calls ToI420().
//
// This target is the only non-FFmpeg place allowed to know about the hw
// decoder -- but even it never includes a libav header (invariant C8); the
// FFmpeg-facing work happens inside avbase_platform_ffmpeg.
class VideoToolboxVideoDecoderFactory final
    : public media::VideoDecoderFactory {
 public:
  explicit VideoToolboxVideoDecoderFactory(
      base::scoped_refptr<base::SequencedTaskRunner> task_runner,
      media::HwCodecMask allowed_codecs);
  VideoToolboxVideoDecoderFactory(const VideoToolboxVideoDecoderFactory&) =
      delete;
  VideoToolboxVideoDecoderFactory& operator=(
      const VideoToolboxVideoDecoderFactory&) = delete;

  media::VideoDecoderCapability GetCapability() const override;
  bool SupportsCodec(media::VideoDecoderType type_hint) const override;
  std::unique_ptr<media::VideoDecoder> CreateVideoDecoder(
      const media::VideoDecoderConfig& config) override;
  const char* name() const override { return "videotoolbox"; }

 private:
  friend class base::RefCountedThreadSafe<VideoDecoderFactory>;
  ~VideoToolboxVideoDecoderFactory() override = default;

  base::scoped_refptr<base::SequencedTaskRunner> task_runner_;
  media::HwCodecMask allowed_codecs_;
};

}  // namespace avbase::platform::videotoolbox

#endif  // AVBASE_PLATFORM_VIDEOTOOLBOX_VIDEOTOOLBOX_VIDEO_DECODER_FACTORY_H_
