// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/video_convert.h"

#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

#if defined(AVBASE_HAVE_LIBYUV)
#include <libyuv.h>
#endif

namespace avbase::media::ffmpeg {

namespace {

// Same source and destination geometry: the fast paths below are pure format
// conversions. Any scaling request goes to sws, which resamples properly.
bool SameGeometry(const AVFrame& src, int dst_w, int dst_h) {
  return src.width == dst_w && src.height == dst_h;
}

// libyuv requires top-down frames: negative stride (bottom-up input, e.g.
// from GL readbacks) would be walked off the buffer.
bool StridesTopDown(const AVFrame& src) {
  return src.linesize[0] >= 0 && src.linesize[1] >= 0 && src.linesize[2] >= 0;
}

#if defined(AVBASE_HAVE_LIBYUV)

// Maps an AVFrame's declared colorspace/range onto libyuv's YuvConstants.
// Returns nullptr for combinations libyuv cannot express (full-range BT.709/
// BT.2020 have no constants in the pinned release); the caller then falls
// back to sws rather than produce shifted color. Unmarked sources follow the
// common convention: limited-range BT.601 for SD-ish content, which matches
// what sws assumes for coefficient-less frames.
const libyuv::YuvConstants* SelectYuvMatrix(const AVFrame& src) {
  const bool full_range = src.color_range == AVCOL_RANGE_JPEG;
  switch (src.colorspace) {
  case AVCOL_SPC_BT709:
    return full_range ? nullptr : &libyuv::kYuvH709Constants;
  case AVCOL_SPC_BT2020_NCL:
  case AVCOL_SPC_BT2020_CL:
    return full_range ? nullptr : &libyuv::kYuv2020Constants;
  case AVCOL_SPC_BT470BG:
  case AVCOL_SPC_SMPTE170M:
  case AVCOL_SPC_SMPTE240M:
    return full_range ? &libyuv::kYuvJPEGConstants : &libyuv::kYuvI601Constants;
  default:
    return full_range ? &libyuv::kYuvJPEGConstants : &libyuv::kYuvI601Constants;
  }
}

#endif  // defined(AVBASE_HAVE_LIBYUV)

}  // namespace

AVPixelFormat AvPixelFormatFromVideoFormat(media::VideoFormat format) {
  switch (format) {
  case media::VideoFormat::kI420:
  case media::VideoFormat::kYV12:
    return AV_PIX_FMT_YUV420P;
  case media::VideoFormat::kNV12:
    return AV_PIX_FMT_NV12;
  case media::VideoFormat::kNV21:
    return AV_PIX_FMT_NV21;
  case media::VideoFormat::kARGB:
    return AV_PIX_FMT_0RGB32;
  case media::VideoFormat::kRGB24:
    return AV_PIX_FMT_RGB24;
  case media::VideoFormat::kRGB565:
    return AV_PIX_FMT_RGB565LE;
  case media::VideoFormat::kP010:
    return AV_PIX_FMT_P010LE;
  case media::VideoFormat::kYUY2:
  case media::VideoFormat::kYUV420P10:
  case media::VideoFormat::kUnknown:
    break;
  }
  return AV_PIX_FMT_NONE;
}

VideoConverter::VideoConverter() = default;

VideoConverter::~VideoConverter() = default;

bool VideoConverter::Configure(int src_w, int src_h, AVPixelFormat src_format,
                               int dst_w, int dst_h, AVPixelFormat dst_format) {
  if (configured_ && src_w == src_w_ && src_h == src_h_ &&
      src_format == src_format_ && dst_w == dst_w_ && dst_h == dst_h_ &&
      dst_format == dst_format_) {
    return true;
  }
  sws_.reset();
  // The fallback context is created eagerly: if the pair is broken we want
  // Configure() to fail up front rather than every Convert() frame.
  sws_ =
      SwsPtr(sws_getContext(src_w, src_h, src_format, dst_w, dst_h, dst_format,
                            SWS_BILINEAR, nullptr, nullptr, nullptr));
  if (!sws_) {
    configured_ = false;
    return false;
  }
  src_w_ = src_w;
  src_h_ = src_h;
  src_format_ = src_format;
  dst_w_ = dst_w;
  dst_h_ = dst_h;
  dst_format_ = dst_format;
  configured_ = true;
  return true;
}

void VideoConverter::Reset() {
  sws_.reset();
  configured_ = false;
}

bool VideoConverter::Convert(const AVFrame& src, uint8_t* const dst_data[4],
                             const int dst_linesize[4]) {
  if (!configured_ || src.width != src_w_ || src.height != src_h_ ||
      src.format != src_format_) {
    return false;
  }
  if (TryLibyuv(src, dst_data, dst_linesize)) {
    return true;
  }
  return ConvertSws(src, dst_data, dst_linesize);
}

bool VideoConverter::TryLibyuv(const AVFrame& src, uint8_t* const dst_data[4],
                               const int dst_linesize[4]) {
#if !defined(AVBASE_HAVE_LIBYUV)
  (void)src;
  (void)dst_data;
  (void)dst_linesize;
#else
  if (!SameGeometry(src, dst_w_, dst_h_) || !StridesTopDown(src)) {
    return false;
  }

  // ---- YUV → YUV: plane-layout conversions only, no color transform, so no
  // matrix selection is involved and the result is exact.
  if (src_format_ == AV_PIX_FMT_NV12 && dst_format_ == AV_PIX_FMT_YUV420P) {
    return libyuv::NV12ToI420(src.data[0], src.linesize[0], src.data[1],
                              src.linesize[1], dst_data[0], dst_linesize[0],
                              dst_data[1], dst_linesize[1], dst_data[2],
                              dst_linesize[2], dst_w_, dst_h_) == 0;
  }
  if (src_format_ == AV_PIX_FMT_NV21 && dst_format_ == AV_PIX_FMT_YUV420P) {
    return libyuv::NV21ToI420(src.data[0], src.linesize[0], src.data[1],
                              src.linesize[1], dst_data[0], dst_linesize[0],
                              dst_data[1], dst_linesize[1], dst_data[2],
                              dst_linesize[2], dst_w_, dst_h_) == 0;
  }
  if (src_format_ == AV_PIX_FMT_YUV422P && dst_format_ == AV_PIX_FMT_YUV420P) {
    return libyuv::I422ToI420(src.data[0], src.linesize[0], src.data[1],
                              src.linesize[1], src.data[2], src.linesize[2],
                              dst_data[0], dst_linesize[0], dst_data[1],
                              dst_linesize[1], dst_data[2], dst_linesize[2],
                              dst_w_, dst_h_) == 0;
  }
  if (src_format_ == AV_PIX_FMT_YUV444P && dst_format_ == AV_PIX_FMT_YUV420P) {
    return libyuv::I444ToI420(src.data[0], src.linesize[0], src.data[1],
                              src.linesize[1], src.data[2], src.linesize[2],
                              dst_data[0], dst_linesize[0], dst_data[1],
                              dst_linesize[1], dst_data[2], dst_linesize[2],
                              dst_w_, dst_h_) == 0;
  }
  // YUVJ420P is BT.601 full range: libyuv's J-prefixed kernels fold the range
  // compression in, so the output is standard limited-range I420.
  if (src_format_ == AV_PIX_FMT_YUVJ420P && dst_format_ == AV_PIX_FMT_YUV420P) {
    return libyuv::J420ToI420(src.data[0], src.linesize[0], src.data[1],
                              src.linesize[1], src.data[2], src.linesize[2],
                              dst_data[0], dst_linesize[0], dst_data[1],
                              dst_linesize[1], dst_data[2], dst_linesize[2],
                              dst_w_, dst_h_) == 0;
  }
  // 10-bit sources: libyuv's 16-bit APIs take strides in ELEMENTS (uint16),
  // hence the linesize / 2.
  if (src_format_ == AV_PIX_FMT_YUV420P10LE &&
      dst_format_ == AV_PIX_FMT_YUV420P) {
    return libyuv::I010ToI420(reinterpret_cast<const uint16_t*>(src.data[0]),
                              src.linesize[0] / 2,
                              reinterpret_cast<const uint16_t*>(src.data[1]),
                              src.linesize[1] / 2,
                              reinterpret_cast<const uint16_t*>(src.data[2]),
                              src.linesize[2] / 2, dst_data[0], dst_linesize[0],
                              dst_data[1], dst_linesize[1], dst_data[2],
                              dst_linesize[2], dst_w_, dst_h_) == 0;
  }
  if (src_format_ == AV_PIX_FMT_YUV422P10LE &&
      dst_format_ == AV_PIX_FMT_YUV420P) {
    return libyuv::I210ToI420(reinterpret_cast<const uint16_t*>(src.data[0]),
                              src.linesize[0] / 2,
                              reinterpret_cast<const uint16_t*>(src.data[1]),
                              src.linesize[1] / 2,
                              reinterpret_cast<const uint16_t*>(src.data[2]),
                              src.linesize[2] / 2, dst_data[0], dst_linesize[0],
                              dst_data[1], dst_linesize[1], dst_data[2],
                              dst_linesize[2], dst_w_, dst_h_) == 0;
  }

  // ---- I420 → packed RGB: byte-order note, verified against libyuv's row
  // kernels (YuvPixel's first output parameter is B, not R). libyuv names
  // packed formats by their little-endian uint32 value, FFmpeg by memory byte
  // order, so the families map across:
  //   FFmpeg BGRA / 0RGB32 (memory B,G,R,A) ← libyuv ARGB kernels
  //   FFmpeg ABGR           (memory A,B,G,R) ← libyuv RGBA kernels
  //   FFmpeg RGBA           (memory R,G,B,A) ← libyuv ABGR kernels
  // The Matrix kernels carry the frame's colorspace. The pinned libyuv has no
  // ABGR matrix kernel, so RGBA is only accelerated for unmarked/BT.601
  // limited-range frames and falls back to sws otherwise. 0RGB32 is BGR0
  // (memory B,G,R,X) only on little-endian, where the ARGB kernel's forced
  // alpha lands in the byte FFmpeg leaves undefined.
  const bool little_endian =
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
      __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#else
      false;
#endif
  if (src_format_ == AV_PIX_FMT_YUV420P &&
      (dst_format_ == AV_PIX_FMT_BGRA ||
       (little_endian && dst_format_ == AV_PIX_FMT_0RGB32))) {
    const libyuv::YuvConstants* matrix = SelectYuvMatrix(src);
    if (!matrix) {
      return false;
    }
    return libyuv::I420ToARGBMatrix(
               src.data[0], src.linesize[0], src.data[1], src.linesize[1],
               src.data[2], src.linesize[2], dst_data[0], dst_linesize[0],
               matrix, dst_w_, dst_h_) == 0;
  }
  if (src_format_ == AV_PIX_FMT_YUV420P && dst_format_ == AV_PIX_FMT_ABGR) {
    const libyuv::YuvConstants* matrix = SelectYuvMatrix(src);
    if (!matrix) {
      return false;
    }
    return libyuv::I420ToRGBAMatrix(
               src.data[0], src.linesize[0], src.data[1], src.linesize[1],
               src.data[2], src.linesize[2], dst_data[0], dst_linesize[0],
               matrix, dst_w_, dst_h_) == 0;
  }
  if (src_format_ == AV_PIX_FMT_YUV420P && dst_format_ == AV_PIX_FMT_RGBA &&
      SelectYuvMatrix(src) == &libyuv::kYuvI601Constants) {
    return libyuv::I420ToABGR(src.data[0], src.linesize[0], src.data[1],
                              src.linesize[1], src.data[2], src.linesize[2],
                              dst_data[0], dst_linesize[0], dst_w_,
                              dst_h_) == 0;
  }
#endif  // defined(AVBASE_HAVE_LIBYUV)
  return false;
}

bool VideoConverter::ConvertSws(const AVFrame& src, uint8_t* const dst_data[4],
                                const int dst_linesize[4]) {
  if (!sws_) {
    return false;
  }
  // YUV → YUV with matching range is a pure plane-layout change; setting
  // colorspace details there pushes sws through an 8-bit RGB intermediate
  // (it logs "yuv422p to bgr24" and requantizes Y through it), wrecking
  // accuracy for zero benefit. Details only matter when a color transform is
  // actually involved: YUV↔RGB, or a full-range source that needs range
  // compression.
  static const auto is_yuv = [](AVPixelFormat f) {
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(f);
    return desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB);
  };
  const bool color_change = !is_yuv(src_format_) || !is_yuv(dst_format_) ||
                            src.color_range == AVCOL_RANGE_JPEG;
  if (color_change) {
    // Per-frame colorspace: the context is geometry-cached, but two frames
    // from the same stream can disagree about matrix/range, and sws defaults
    // to BT.601 regardless of what the frame declares.
    int coefficients = SWS_CS_ITU709;
    switch (src.colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
      coefficients = SWS_CS_ITU601;
      break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
      coefficients = SWS_CS_BT2020;
      break;
    default:
      break;
    }
    const int src_range = (src.color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    sws_setColorspaceDetails(sws_.get(), sws_getCoefficients(coefficients),
                             src_range, sws_getCoefficients(SWS_CS_DEFAULT), 0,
                             0, 1 << 16, 1 << 16);
  }
  return sws_scale(sws_.get(), src.data, src.linesize, 0, src.height, dst_data,
                   dst_linesize) == src.height;
}

}  // namespace avbase::media::ffmpeg
