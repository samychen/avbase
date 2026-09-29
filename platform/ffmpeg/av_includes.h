// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// THE ONLY PLACE IN IJKPP THAT INCLUDES FFmpeg HEADERS.
//
// Two jobs:
//   1. Wrap the C headers in extern "C". FFmpeg's own headers do not do this,
//      so including them from C++ without the wrapper produces link errors that
//      look nothing like their cause.
//   2. Derive every FFmpeg version switch in the project. check_invariants rule
//      C8 fails the build if `#if LIBAV*_VERSION` appears anywhere else, so all
//      4.4-to-8.x differences are reviewable in one file.

#ifndef IJKPP_PLATFORM_FFMPEG_AV_INCLUDES_H_
#define IJKPP_PLATFORM_FFMPEG_AV_INCLUDES_H_

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// FFmpeg 5.1 (libavutil 57.28) replaced channels/channel_layout with the
// AVChannelLayout struct. This is the single largest source of 4.x/5.x+ churn.
#if (LIBAVUTIL_VERSION_MAJOR > 57) || \
    (LIBAVUTIL_VERSION_MAJOR == 57 && LIBAVUTIL_VERSION_MINOR >= 28)
#define IJKPP_FFMPEG_HAS_CHANNEL_LAYOUT 1
#else
#define IJKPP_FFMPEG_HAS_CHANNEL_LAYOUT 0
#endif

// FFmpeg 4.4 (libavcodec 58) widened AVPacket's pts/dts/duration to int64_t.
#if LIBAVCODEC_VERSION_MAJOR >= 58
#define IJKPP_FFMPEG_PACKET_INT64 1
#else
#define IJKPP_FFMPEG_PACKET_INT64 0
#endif

// FFmpeg 7.x (libavcodec 61): ticks_per_frame deprecated, various cleanups.
#if LIBAVCODEC_VERSION_MAJOR >= 61
#define IJKPP_FFMPEG_7_OR_NEWER 1
#else
#define IJKPP_FFMPEG_7_OR_NEWER 0
#endif

// av_dict_iterate() was added in libavutil 58 (FFmpeg 6.0). Earlier releases
// must iterate with av_dict_get(..., AV_DICT_IGNORE_SUFFIX).
#if LIBAVUTIL_VERSION_MAJOR >= 58
#define IJKPP_FFMPEG_HAS_DICT_ITERATE 1
#else
#define IJKPP_FFMPEG_HAS_DICT_ITERATE 0
#endif

// av_register_all() was removed in FFmpeg 4.0; guard so the glue never calls it.
#if LIBAVFORMAT_VERSION_MAJOR < 58
#define IJKPP_FFMPEG_NEEDS_REGISTER_ALL 1
#else
#define IJKPP_FFMPEG_NEEDS_REGISTER_ALL 0
#endif

#endif  // IJKPP_PLATFORM_FFMPEG_AV_INCLUDES_H_
