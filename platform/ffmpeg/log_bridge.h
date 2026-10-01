// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_FFMPEG_LOG_BRIDGE_H_
#define AVBASE_PLATFORM_FFMPEG_LOG_BRIDGE_H_

#include <string>

#include "base/logging.h"
#include "platform/ffmpeg/av_includes.h"

namespace avbase::platform::ffmpeg {

// Routes av_log() into base/logging, so a host that installed a
// base::logging::LoggingDelegate sees FFmpeg's messages alongside avbase's.
//
// ijkplayer hard-codes __android_log_print in ijksdl_android_jni.c and writes
// to stderr everywhere else; this is the platform-independent replacement
// (behaviour difference Δ22).
//
// Install once from FFmpegGlue::InitializeFFmpeg(). Idempotent.
void InstallLogBridge();

// Maps an AV_LOG_* level onto avbase's severities. Exposed for testing.
base::logging::LogSeverity MapAvLogLevel(int av_level);

// Version string of the linked FFmpeg, e.g. "7.1.1".
std::string GetFFmpegVersionString();

}  // namespace avbase::platform::ffmpeg

#endif  // AVBASE_PLATFORM_FFMPEG_LOG_BRIDGE_H_
