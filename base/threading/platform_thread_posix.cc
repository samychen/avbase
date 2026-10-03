// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/threading/platform_thread.h"

#if defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif

namespace avbase::base::PlatformThread {
namespace {

// Truncate to the lowest common denominator across the POSIX targets: Linux's
// 16-byte comm limit. Apple allows 64, but keeping every platform at the same
// width is what makes the docs/04 thread table (avbase-media, avbase-demux,
// ...) read identically in logs on both, and the Δ16 names all fit either way.
constexpr size_t kMaxThreadNameLen = 15;

}  // namespace

void SetName(const std::string& name) {
#if defined(__linux__)
  const std::string truncated = name.substr(0, kMaxThreadNameLen);
  pthread_setname_np(pthread_self(), truncated.c_str());
#elif defined(__APPLE__)
  // The single-argument Apple variant names the *current* thread, which is
  // exactly the contract: Thread::Start() calls this from the new thread
  // before it runs any task.
  const std::string truncated = name.substr(0, kMaxThreadNameLen);
  pthread_setname_np(truncated.c_str());
#else
  (void)name;
#endif
}

std::string GetName() {
#if defined(__linux__) || defined(__APPLE__)
  char buf[64] = {0};
  if (pthread_getname_np(pthread_self(), buf, sizeof(buf)) == 0) {
    return std::string(buf);
  }
#endif
  return {};
}

void SetCurrentThreadPriority(ThreadPriority priority) {
#if defined(__linux__)
  // SCHED_OTHER nice values. Audio gets the most favourable setting because an
  // underrun is audible, whereas a late video frame is merely visible.
  int nice_value = 0;
  switch (priority) {
  case ThreadPriority::kBackground:
    nice_value = 10;
    break;
  case ThreadPriority::kBestEffort:
    nice_value = 0;
    break;
  case ThreadPriority::kDisplayCritical:
    nice_value = -6;
    break;
  }
  // Requires CAP_SYS_NICE for negative values; failure is expected and benign
  // for unprivileged processes, so it is deliberately not checked.
  setpriority(PRIO_PROCESS, 0, nice_value);
#else
  (void)priority;
#endif
}

}  // namespace avbase::base::PlatformThread
