// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `base/threading/platform_thread.h` (BSD-3-Clause).

#ifndef AVBASE_BASE_THREADING_PLATFORM_THREAD_H_
#define AVBASE_BASE_THREADING_PLATFORM_THREAD_H_

#include <string>

#include "base/base_export.h"

namespace avbase::base {

enum class ThreadPriority {
  kBackground,
  kBestEffort,
  kDisplayCritical,  // Audio and video render threads.
};

namespace PlatformThread {

// Sets the OS-visible name of the calling thread. Linux truncates to 15
// characters, so avbase names stay short: "avbase-media", "avbase-demux",
// "avbase-video", "avbase-audio", "avbase-event".
AVBASE_BASE_EXPORT void SetName(const std::string& name);
AVBASE_BASE_EXPORT std::string GetName();
AVBASE_BASE_EXPORT void SetCurrentThreadPriority(ThreadPriority priority);

}  // namespace PlatformThread
}  // namespace avbase::base

#endif  // AVBASE_BASE_THREADING_PLATFORM_THREAD_H_
