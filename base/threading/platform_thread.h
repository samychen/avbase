// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `base/threading/platform_thread.h` (BSD-3-Clause).

#ifndef IJKPP_BASE_THREADING_PLATFORM_THREAD_H_
#define IJKPP_BASE_THREADING_PLATFORM_THREAD_H_

#include <string>

#include "base/base_export.h"

namespace ijkpp::base {

enum class ThreadPriority {
  kBackground,
  kBestEffort,
  kDisplayCritical,   // Audio and video render threads.
};

namespace PlatformThread {

// Sets the OS-visible name of the calling thread. Linux truncates to 15
// characters, so ijkpp names stay short: "ijkpp-media", "ijkpp-demux",
// "ijkpp-video", "ijkpp-audio", "ijkpp-event".
IJKPP_BASE_EXPORT void SetName(const std::string& name);
IJKPP_BASE_EXPORT std::string GetName();
IJKPP_BASE_EXPORT void SetCurrentThreadPriority(ThreadPriority priority);

}  // namespace PlatformThread
}  // namespace base

#endif  // IJKPP_BASE_THREADING_PLATFORM_THREAD_H_
