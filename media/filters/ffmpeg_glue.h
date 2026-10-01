// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/filters/ffmpeg_glue.h` (BSD-3-Clause).

#ifndef AVBASE_MEDIA_FILTERS_FFMPEG_GLUE_H_
#define AVBASE_MEDIA_FILTERS_FFMPEG_GLUE_H_

#include <string>

#include "media/media_export.h"

namespace avbase::media::ffmpeg {

// One-time FFmpeg initialisation: installs the log bridge (av_log ->
// base/logging) and, on FFmpeg < 4.0, registers protocols. Idempotent and
// thread-safe; called from player::GlobalInit() and from FFmpegDemuxer.
AVBASE_MEDIA_EXPORT void InitializeFFmpeg();

// e.g. "libavformat 61.7.100 / libavcodec 61.19.101".
AVBASE_MEDIA_EXPORT std::string GetFFmpegVersionString();

// True once InitializeFFmpeg() has completed.
AVBASE_MEDIA_EXPORT bool IsFFmpegInitialized();

// Configuration string describing which demuxers/decoders/protocols this FFmpeg
// build actually has. Used by `avbase-inspect doctor` so a missing component is
// reported as "not compiled into your FFmpeg" instead of surfacing later as an
// opaque open failure (docs/10 §10.2).
AVBASE_MEDIA_EXPORT std::string GetFFmpegConfigurationSummary();

}  // namespace avbase::media::ffmpeg

#endif  // AVBASE_MEDIA_FILTERS_FFMPEG_GLUE_H_
