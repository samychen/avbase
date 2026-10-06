// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Pixel-format conversion for the FFmpeg decode path: libyuv fast paths for
// the format pairs that dominate playback, sws_scale for everything else.
//
// Why not plain sws_scale everywhere: swscale is scalar-correct but slow for
// the hot pairs (NV12→I420 from hw decoders, 422/10-bit broadcast sources →
// I420, I420→RGB for renderers); libyuv's SIMD kernels are several times
// faster. Why not libyuv everywhere: it only speaks a fixed set of format
// pairs, its non-matrix YUV→RGB kernels are hard-wired to BT.601, and it
// cannot handle negative strides. So the dispatcher sends each format pair to
// whichever backend is both correct and fast, and the sws fallback carries
// the colorspace details the fast paths cannot express.
//
// Everything here stays inside the platform/ffmpeg vendor quarantine (C4):
// callers hand over AVFrame data/linesize arrays and never see FFmpeg types.

#ifndef AVBASE_PLATFORM_FFMPEG_VIDEO_CONVERT_H_
#define AVBASE_PLATFORM_FFMPEG_VIDEO_CONVERT_H_

#include "media/base/video_frame.h"
#include "media/media_export.h"
#include "platform/ffmpeg/av_includes.h"
#include "platform/ffmpeg/compat.h"

namespace avbase::platform::ffmpeg {

// The VideoFormat -> AVPixelFormat table, shared by every FFmpeg-side
// consumer of avbase frames (was a private helper of the video decoder).
AVBASE_MEDIA_EXPORT AVPixelFormat
AvPixelFormatFromVideoFormat(media::VideoFormat format);

// Converts one video frame per call. Configure() pins the geometry; Convert()
// fills caller-owned destination planes (AVFrame data/linesize convention:
// up to 4 entries, entries beyond the format's plane count are ignored).
//
// AVBASE_HAVE_LIBYUV (set by the build when libyuv was found) enables the
// SIMD fast paths; without it the class is a thin cached-sws wrapper with
// identical observable behavior.
class VideoConverter {
 public:
  VideoConverter();
  ~VideoConverter();
  VideoConverter(const VideoConverter&) = delete;
  VideoConverter& operator=(const VideoConverter&) = delete;

  // Pins src→dst geometry. A no-op when unchanged; rebuilds the sws fallback
  // context otherwise. Returns false only if the fallback context cannot be
  // created (a malformed format pair) — fast-path-capable pairs still need a
  // valid fallback here, because Convert() may hit it for any frame (e.g.
  // negative stride, unsupported colorspace).
  bool Configure(int src_w, int src_h, AVPixelFormat src_format, int dst_w,
                 int dst_h, AVPixelFormat dst_format);

  // True between a successful Configure() and Reset().
  bool Configured() const { return configured_; }
  void Reset();

  // Converts |src| into |dst_data|/|dst_linesize|. Returns false when |src|
  // does not match the configured geometry or the backend fails. The frame's
  // colorspace/range is honored on every path (libyuv via matrix constants,
  // sws via sws_setColorspaceDetails).
  bool Convert(const AVFrame& src, uint8_t* const dst_data[4],
               const int dst_linesize[4]);

 private:
  bool TryLibyuv(const AVFrame& src, uint8_t* const dst_data[4],
                 const int dst_linesize[4]);
  bool ConvertSws(const AVFrame& src, uint8_t* const dst_data[4],
                  const int dst_linesize[4]);

  int src_w_{0};
  int src_h_{0};
  AVPixelFormat src_format_{AV_PIX_FMT_NONE};
  int dst_w_{0};
  int dst_h_{0};
  AVPixelFormat dst_format_{AV_PIX_FMT_NONE};
  bool configured_{false};

  // Lazily built by Configure(); kept across frames for the fallback path.
  SwsPtr sws_;
};

}  // namespace avbase::platform::ffmpeg

#endif  // AVBASE_PLATFORM_FFMPEG_VIDEO_CONVERT_H_
