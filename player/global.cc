// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/global.h"

#include <mutex>

#include "avbase/Version.h"

// FFmpeg initialisation is wired in at milestone M4, when
// media/ffmpeg/ffmpeg_glue.cc lands. Until then GlobalInit() only guards the
// once_flag, which keeps this translation unit independent of FFmpeg.

namespace avbase {
namespace {

std::once_flag g_init_once;

void DoGlobalInit() {
  // XInitThreads() must run before the host's first X11 call; the Linux backend
  // performs it lazily under the same once_flag when it loads libX11.
}

}  // namespace

void GlobalInit() {
  std::call_once(g_init_once, &DoGlobalInit);
}

const char* GetVersion() {
  return AVBASE_VERSION_STRING;
}

const char* GetFFmpegVersion() {
  // Reports the build-time FFmpeg version once M4 lands; "none" for a core-only
  // build, which is the configuration CI uses to prove goal G2.
  return "none";
}

}  // namespace avbase
