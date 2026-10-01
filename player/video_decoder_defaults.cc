// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/video_decoder_defaults.h"

#include "avbase/BuildConfig.h"
#include "base/memory/scoped_refptr.h"
#if AVBASE_ENABLE_D3D11
#include "platform/d3d11/d3d11_video_decoder_factory.h"
#endif
#if AVBASE_ENABLE_VAAPI
#include "platform/vaapi/vaapi_video_decoder_factory.h"
#endif
#if AVBASE_ENABLE_VIDEOTOOLBOX
#include "platform/videotoolbox/videotoolbox_video_decoder_factory.h"
#endif

namespace avbase {

std::vector<base::scoped_refptr<media::VideoDecoderFactory>>
DefaultHardwareVideoDecoderFactories(
    const base::scoped_refptr<base::SequencedTaskRunner>& video_runner,
    media::HwCodecMask allowed_codecs) {
  std::vector<base::scoped_refptr<media::VideoDecoderFactory>> out;
#if AVBASE_ENABLE_VIDEOTOOLBOX
  out.push_back(base::MakeRefCounted<
                platform::videotoolbox::VideoToolboxVideoDecoderFactory>(
      video_runner, allowed_codecs));
#endif
#if AVBASE_ENABLE_VAAPI
  out.push_back(
      base::MakeRefCounted<platform::vaapi::VaapiVideoDecoderFactory>(
          video_runner, allowed_codecs));
#endif
#if AVBASE_ENABLE_D3D11
  out.push_back(
      base::MakeRefCounted<platform::d3d11::D3D11VideoDecoderFactory>(
          video_runner, allowed_codecs));
#endif
  return out;
}

}  // namespace avbase
