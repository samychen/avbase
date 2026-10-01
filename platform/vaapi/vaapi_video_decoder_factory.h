// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_VAAPI_VAAPI_VIDEO_DECODER_FACTORY_H_
#define AVBASE_PLATFORM_VAAPI_VAAPI_VIDEO_DECODER_FACTORY_H_

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/video_decoder_factory.h"
#include "media/filters/decoder_selector.h"

namespace avbase::platform::vaapi {

// Linux VAAPI hardware decoder factory (FFmpeg-hwaccel mediated). Frames
// leave the decoder as VASurfaceID-backed VideoFrames
// (NativeHandleKind::kVaapiSurface); EGL import into a consumer happens at
// the sink/display layer, pixels only on explicit ToI420().
//
// Phase 3 ships this as the Linux hardware candidate; the sink-side
// VASurface→EGLImage path (dmabuf export) is the follow-up that completes a
// full zero-copy DISPLAY chain -- decode is zero-copy from this factory on.
class VaapiVideoDecoderFactory final : public media::VideoDecoderFactory {
 public:
  explicit VaapiVideoDecoderFactory(
      base::scoped_refptr<base::SequencedTaskRunner> task_runner,
      media::HwCodecMask allowed_codecs);
  VaapiVideoDecoderFactory(const VaapiVideoDecoderFactory&) = delete;
  VaapiVideoDecoderFactory& operator=(const VaapiVideoDecoderFactory&) = delete;

  media::VideoDecoderCapability GetCapability() const override;
  bool SupportsCodec(media::VideoDecoderType type_hint) const override;
  std::unique_ptr<media::VideoDecoder> CreateVideoDecoder(
      const media::VideoDecoderConfig& config) override;
  const char* name() const override { return "vaapi"; }

 private:
  friend class base::RefCountedThreadSafe<VideoDecoderFactory>;
  ~VaapiVideoDecoderFactory() override = default;

  base::scoped_refptr<base::SequencedTaskRunner> task_runner_;
  media::HwCodecMask allowed_codecs_;
};

}  // namespace avbase::platform::vaapi

#endif  // AVBASE_PLATFORM_VAAPI_VAAPI_VIDEO_DECODER_FACTORY_H_
