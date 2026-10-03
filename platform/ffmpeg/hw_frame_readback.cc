// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/ffmpeg/hw_frame_readback.h"

#include "platform/ffmpeg/av_includes.h"
#include "platform/ffmpeg/compat.h"

namespace avbase::platform::ffmpeg {

base::scoped_refptr<media::VideoFrame>
MapHwFrameToI420(const AVFrame* hw_frame, media::Rational sar,
                 base::TimeDelta timestamp, base::TimeDelta duration,
                 int32_t serial, media::VideoColorSpace cs) {
  AVFrame* sw_raw = av_frame_alloc();
  if (!sw_raw) {
    return nullptr;
  }
  FramePtr sw_frame(sw_raw);
  // The hw→sw transfer. The frames context decides the CPU layout; we do not
  // pick one, because pretending to know the device's layout is how a driver
  // update turns into corrupted chroma.
  if (av_hwframe_transfer_data(sw_frame.get(), const_cast<AVFrame*>(hw_frame),
                               0) < 0) {
    return nullptr;
  }

  const int width = sw_frame->width;
  const int height = sw_frame->height;
  auto out = media::VideoFrame::CreateBlackFrame(
      media::VideoFormat::kI420, media::Size{width, height},
      media::Size{width, height}, sar, timestamp, duration, serial);
  if (!out) {
    return nullptr;
  }
  SwsPtr sws(sws_getContext(
      width, height, static_cast<AVPixelFormat>(sw_frame->format), width,
      height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
  if (!sws) {
    return nullptr;
  }
  uint8_t* dst[4] = {nullptr, nullptr, nullptr, nullptr};
  int dst_stride[4] = {0, 0, 0, 0};
  const int planes = media::VideoFormatPlaneCount(media::VideoFormat::kI420);
  for (int p = 0; p < planes && p < media::VideoFrame::kMaxPlanes; ++p) {
    const auto plane = static_cast<media::VideoFrame::Plane>(p);
    dst[p] = out->mutable_data(plane).data();
    dst_stride[p] = out->stride(plane);
  }
  if (sws_scale(sws.get(), sw_frame->data, sw_frame->linesize, 0, height, dst,
                dst_stride) <= 0) {
    return nullptr;
  }
  out->set_color_space(cs);
  return out;
}

}  // namespace avbase::platform::ffmpeg
