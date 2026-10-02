// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/video_decoder_defaults.h"

#include "avbase/BuildConfig.h"
#include "base/memory/scoped_refptr.h"
#include "platform/ffmpeg/ffmpeg_hw_decoder_factory.h"
#if AVBASE_ENABLE_D3D11
#include "platform/d3d11/d3d11_hw_spec.h"
#endif
#if AVBASE_ENABLE_VAAPI
#include "platform/vaapi/vaapi_hw_spec.h"
#endif
#if AVBASE_ENABLE_VIDEOTOOLBOX
#include "platform/videotoolbox/videotoolbox_hw_spec.h"
#endif

namespace avbase {

std::vector<base::scoped_refptr<media::VideoDecoderFactory>>
DefaultHardwareVideoDecoderFactories(
    const base::scoped_refptr<base::SequencedTaskRunner>& video_runner,
    media::HwCodecMask allowed_codecs) {
  std::vector<base::scoped_refptr<media::VideoDecoderFactory>> out;
  // One factory class, three specs. The #if order is irrelevant: at most one
  // spec compiles on any given host platform.
#if AVBASE_ENABLE_VIDEOTOOLBOX
  using platform::ffmpeg::FFmpegHwVideoDecoderFactory;
  out.push_back(
      base::MakeRefCounted<FFmpegHwVideoDecoderFactory>(
          platform::videotoolbox::VideotoolboxHwSpec(), video_runner,
          allowed_codecs));
#endif
#if AVBASE_ENABLE_VAAPI
  using platform::ffmpeg::FFmpegHwVideoDecoderFactory;
  out.push_back(base::MakeRefCounted<FFmpegHwVideoDecoderFactory>(
      platform::vaapi::VaapiHwSpec(), video_runner, allowed_codecs));
#endif
#if AVBASE_ENABLE_D3D11
  using platform::ffmpeg::FFmpegHwVideoDecoderFactory;
  out.push_back(base::MakeRefCounted<FFmpegHwVideoDecoderFactory>(
      platform::d3d11::D3D11HwSpec(), video_runner, allowed_codecs));
#endif
  return out;
}

}  // namespace avbase
