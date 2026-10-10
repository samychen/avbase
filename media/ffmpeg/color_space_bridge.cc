// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/color_space_bridge.h"

#include "media/ffmpeg/av_includes.h"

namespace avbase::media::ffmpeg {

media::VideoColorSpace
ColorSpaceFromAvFrame(const AVFrame* frame,
                      const media::VideoDecoderConfig& config) {
  media::VideoColorSpace cs;
  switch (frame->colorspace) {
  case AVCOL_SPC_BT709:
    cs.matrix = media::ColorMatrix::kBT709;
    break;
  case AVCOL_SPC_BT470BG:
  case AVCOL_SPC_SMPTE170M:
    cs.matrix = media::ColorMatrix::kSMPTE170M;
    break;
  case AVCOL_SPC_BT2020_NCL:
    cs.matrix = media::ColorMatrix::kBT2020Ncl;
    break;
  case AVCOL_SPC_BT2020_CL:
    cs.matrix = media::ColorMatrix::kBT2020Cl;
    break;
  case AVCOL_SPC_RGB:
    cs.matrix = media::ColorMatrix::kIdentity;
    break;
  default:
    cs.matrix = media::ColorMatrix::kUnknown;
    break;
  }
  switch (frame->color_primaries) {
  case AVCOL_PRI_BT709:
    cs.primaries = media::ColorPrimaries::kBT709;
    break;
  case AVCOL_PRI_BT470BG:
  case AVCOL_PRI_SMPTE170M:
    cs.primaries = media::ColorPrimaries::kSMPTE170M;
    break;
  case AVCOL_PRI_BT2020:
    cs.primaries = media::ColorPrimaries::kBT2020;
    break;
  default:
    cs.primaries = media::ColorPrimaries::kUnknown;
    break;
  }
  switch (frame->color_trc) {
  case AVCOL_TRC_BT709:
    cs.transfer = media::ColorTransfer::kBT709;
    break;
  case AVCOL_TRC_GAMMA22:
    cs.transfer = media::ColorTransfer::kGamma22;
    break;
  case AVCOL_TRC_SMPTE2084:
    cs.transfer = media::ColorTransfer::kSMPTE2084;
    break;
  case AVCOL_TRC_ARIB_STD_B67:
    cs.transfer = media::ColorTransfer::kARIBStdB67;
    break;
  case AVCOL_TRC_IEC61966_2_1:
    cs.transfer = media::ColorTransfer::kSRGB;
    break;
  default:
    cs.transfer = media::ColorTransfer::kUnknown;
    break;
  }
  switch (frame->color_range) {
  case AVCOL_RANGE_MPEG:
    cs.range = media::ColorRange::kLimited;
    break;
  case AVCOL_RANGE_JPEG:
    cs.range = media::ColorRange::kFull;
    break;
  default:
    cs.range = media::ColorRange::kUnknown;
    break;
  }

  if (cs.IsSpecified()) {
    return cs;
  }
  // avbase risk §14.4: FFmpeg does not always propagate the container's colour
  // tags onto decoded frames -- the hwaccel paths in particular frequently
  // hand back "unspecified". The container-level facts (codec, size, HDR flag)
  // are what the muxer actually recorded, so fall back to the table keyed on
  // those rather than publishing a frame with no colour story at all.
  return media::GuessColorSpaceFallback(config.coded_size.width,
                                        config.coded_size.height,
                                        config.has_hdr_metadata);
}

}  // namespace avbase::media::ffmpeg
