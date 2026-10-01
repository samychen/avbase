// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_D3D11_D3D11_VIDEO_DECODER_FACTORY_H_
#define AVBASE_PLATFORM_D3D11_D3D11_VIDEO_DECODER_FACTORY_H_

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/video_decoder_factory.h"
#include "media/filters/decoder_selector.h"

namespace avbase::platform::d3d11 {

// Windows D3D11 hardware decoder factory (FFmpeg-hwaccel mediated; NVDEC and
// QuickSync both surface through D3D11VA). Frames leave the decoder as
// ID3D11Texture2D-backed VideoFrames with the subresource index in the typed
// NativeHandle (NativeHandleKind::kD3D11Texture); the D3D11-GL interop
// consumer (Phase 3 acceptance) imports the texture at the display layer.
class D3D11VideoDecoderFactory final : public media::VideoDecoderFactory {
 public:
  explicit D3D11VideoDecoderFactory(
      base::scoped_refptr<base::SequencedTaskRunner> task_runner,
      media::HwCodecMask allowed_codecs);
  D3D11VideoDecoderFactory(const D3D11VideoDecoderFactory&) = delete;
  D3D11VideoDecoderFactory& operator=(const D3D11VideoDecoderFactory&) = delete;

  media::VideoDecoderCapability GetCapability() const override;
  bool SupportsCodec(media::VideoDecoderType type_hint) const override;
  std::unique_ptr<media::VideoDecoder> CreateVideoDecoder(
      const media::VideoDecoderConfig& config) override;
  const char* name() const override { return "d3d11"; }

 private:
  friend class base::RefCountedThreadSafe<VideoDecoderFactory>;
  ~D3D11VideoDecoderFactory() override = default;

  base::scoped_refptr<base::SequencedTaskRunner> task_runner_;
  media::HwCodecMask allowed_codecs_;
};

}  // namespace avbase::platform::d3d11

#endif  // AVBASE_PLATFORM_D3D11_D3D11_VIDEO_DECODER_FACTORY_H_
