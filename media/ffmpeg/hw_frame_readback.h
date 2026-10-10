// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_HW_FRAME_READBACK_H_
#define AVBASE_MEDIA_FFMPEG_HW_FRAME_READBACK_H_

#include "base/time/time.h"
#include "media/base/video_color_space.h"
#include "media/base/video_frame.h"

struct AVFrame;

namespace avbase::media::ffmpeg {

// The explicit-readback half of the zero-copy contract (avbase §6.2): copies
// a hardware frame's pixels into a freshly allocated owned I420 VideoFrame.
// Only ever called from behind VideoFrame::ToI420() -- never as part of the
// decode path, which keeps the surface GPU-resident end to end.
//
// |hw_frame| must be an AVFrame whose format is a hardware pixel format;
// av_hwframe_transfer_data pulls it to CPU memory, swscale converts whatever
// the device's software layout is (NV12, P010, ...) to I420. Returns nullptr
// when the transfer or conversion fails; the frame stays valid and the
// consumer can retry.
base::scoped_refptr<media::VideoFrame>
MapHwFrameToI420(const AVFrame* hw_frame, media::Rational sar,
                 base::TimeDelta timestamp, base::TimeDelta duration,
                 int32_t serial, media::VideoColorSpace cs);

}  // namespace avbase::media::ffmpeg

#endif  // AVBASE_MEDIA_FFMPEG_HW_FRAME_READBACK_H_
