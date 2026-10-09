// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// THE ONLY PLACE IN AVBASE THAT INCLUDES FFmpeg HEADERS.
//
// Two jobs:
//   1. Wrap the C headers in extern "C". FFmpeg's own headers do not do this,
//      so including them from C++ without the wrapper produces link errors that
//      look nothing like their cause.
//   2. Derive every FFmpeg version switch in the project. check_invariants rule
//      C8 fails the build if `#if LIBAV*_VERSION` appears anywhere else, so all
//      4.4-to-8.x differences are reviewable in one file.

#ifndef AVBASE_PLATFORM_FFMPEG_AV_INCLUDES_H_
#define AVBASE_PLATFORM_FFMPEG_AV_INCLUDES_H_

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavcodec/packet.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
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
#define AVBASE_FFMPEG_HAS_CHANNEL_LAYOUT 1
#else
#define AVBASE_FFMPEG_HAS_CHANNEL_LAYOUT 0
#endif

// FFmpeg 4.4 (libavcodec 58) widened AVPacket's pts/dts/duration to int64_t.
#if LIBAVCODEC_VERSION_MAJOR >= 58
#define AVBASE_FFMPEG_PACKET_INT64 1
#else
#define AVBASE_FFMPEG_PACKET_INT64 0
#endif

// FFmpeg 7.x (libavcodec 61): ticks_per_frame deprecated, various cleanups.
#if LIBAVCODEC_VERSION_MAJOR >= 61
#define AVBASE_FFMPEG_7_OR_NEWER 1
#else
#define AVBASE_FFMPEG_7_OR_NEWER 0
#endif

// av_dict_iterate() was added in libavutil 58 (FFmpeg 6.0). Earlier releases
// must iterate with av_dict_get(..., AV_DICT_IGNORE_SUFFIX).
#if LIBAVUTIL_VERSION_MAJOR >= 58
#define AVBASE_FFMPEG_HAS_DICT_ITERATE 1
#else
#define AVBASE_FFMPEG_HAS_DICT_ITERATE 0
#endif

// av_register_all() was removed in FFmpeg 4.0; guard so the glue never calls
// it.
#if LIBAVFORMAT_VERSION_MAJOR < 58
#define AVBASE_FFMPEG_NEEDS_REGISTER_ALL 1
#else
#define AVBASE_FFMPEG_NEEDS_REGISTER_ALL 0
#endif

// FFmpeg 6.1 (libavformat 60.16) deprecated av_stream_get_side_data() in favour
// of av_packet_side_data_get() over the stream's codecpar side data.
#if LIBAVFORMAT_VERSION_MAJOR > 60 || \
    (LIBAVFORMAT_VERSION_MAJOR == 60 && LIBAVFORMAT_VERSION_MINOR >= 16)
#define AVBASE_FFMPEG_HAS_CODECPAR_SIDE_DATA 1
#else
#define AVBASE_FFMPEG_HAS_CODECPAR_SIDE_DATA 0
#endif

// Reads a stream's side data of |type|: returns nullptr when it is absent and
// always sets |*size|. One call shape for both sides of the deprecation, so the
// version switch stays in this file (rule C8) and callers read the same either
// way. The modern branch is codecpar's array, which is where FFmpeg 6.1 moved
// stream side data to.
inline const uint8_t* avbase_stream_side_data(const AVStream* stream,
                                              AVPacketSideDataType type,
                                              size_t* size) {
  *size = 0;
#if AVBASE_FFMPEG_HAS_CODECPAR_SIDE_DATA
  if (!stream || !stream->codecpar) {
    return nullptr;
  }
  const AVPacketSideData* side_data =
      av_packet_side_data_get(stream->codecpar->coded_side_data,
                              stream->codecpar->nb_coded_side_data, type);
  if (!side_data) {
    return nullptr;
  }
  *size = side_data->size;
  return side_data->data;
#else
  return av_stream_get_side_data(stream, type, size);
#endif
}

#endif  // AVBASE_PLATFORM_FFMPEG_AV_INCLUDES_H_
