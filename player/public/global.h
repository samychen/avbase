// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_PUBLIC_GLOBAL_H_
#define AVBASE_PLAYER_PUBLIC_GLOBAL_H_

#include "player/public/player_export.h"

namespace avbase {

// One-time, idempotent process-wide initialisation: bridges the vendor log,
// initialises FFmpeg exactly once, and calls XInitThreads() when the process
// will use X11.
//
// Every Player constructor calls this, so you normally never need to. Call it
// explicitly only when you must guarantee it happens before your own first
// X11 call (see docs/09 §10 L1).
//
// Thread safety: safe to call concurrently; guarded by std::call_once.
AVBASE_PLAYER_EXPORT void GlobalInit();

// Version of avbase, e.g. "0.1.0".
AVBASE_PLAYER_EXPORT const char* GetVersion();

// Version of the FFmpeg avbase was built against, e.g. "7.1", or "none" when
// built with AVBASE_ENABLE_FFMPEG=OFF.
AVBASE_PLAYER_EXPORT const char* GetFFmpegVersion();

// RAII helper for programs that want symmetric init/shutdown.
class AVBASE_PLAYER_EXPORT GlobalGuard {
 public:
  GlobalGuard() { GlobalInit(); }
  GlobalGuard(const GlobalGuard&) = delete;
  GlobalGuard& operator=(const GlobalGuard&) = delete;
  ~GlobalGuard() = default;
};

}  // namespace avbase

#endif  // AVBASE_PLAYER_PUBLIC_GLOBAL_H_
