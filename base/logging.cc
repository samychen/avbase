// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/logging.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace avbase::base::logging {
namespace {

std::atomic<LogSeverity> g_min_log_level{LOG_INFO};
std::atomic<int> g_min_vlog_level{0};
// Function-local statics rather than heap-allocated leaks: logging can happen
// up to the very end of main(), but never from a static destructor in avbase,
// and LeakSanitizer cleanliness is a release blocker (docs/07 §9.1).
std::mutex& DelegateMutex() {
  static std::mutex mutex;
  return mutex;
}
LoggingDelegate*& DelegateSlot() {
  static LoggingDelegate* slot = nullptr;
  return slot;
}

const char* SeverityName(LogSeverity severity) {
  switch (severity) {
    case LOG_VERBOSE: return "VERBOSE";
    case LOG_INFO:    return "INFO";
    case LOG_WARNING: return "WARNING";
    case LOG_ERROR:   return "ERROR";
    case LOG_FATAL:   return "FATAL";
  }
  return "UNKNOWN";
}

}  // namespace

void SetDelegate(std::unique_ptr<LoggingDelegate> delegate) {
  std::lock_guard<std::mutex> lock(DelegateMutex());
  delete DelegateSlot();
  DelegateSlot() = delegate.release();
}

void SetMinLogLevel(LogSeverity severity) { g_min_log_level.store(severity); }
LogSeverity GetMinLogLevel() { return g_min_log_level.load(); }
bool ShouldCreateLogMessage(LogSeverity severity) {
  return severity >= g_min_log_level.load(std::memory_order_relaxed);
}
void SetMinVLogLevel(int level) { g_min_vlog_level.store(level); }
int GetMinVLogLevel() { return g_min_vlog_level.load(); }
bool ShouldCreateVerboseMessage(int verbosity) {
  return verbosity <= g_min_vlog_level.load(std::memory_order_relaxed);
}

LogMessage::LogMessage(const char* file, int line, LogSeverity severity)
    : file_(file), line_(line), severity_(severity) {}

LogMessage::~LogMessage() {
  const std::string text = stream_.str();

  LoggingDelegate* delegate = nullptr;
  {
    std::lock_guard<std::mutex> lock(DelegateMutex());
    delegate = DelegateSlot();
  }
  if (delegate) {
    delegate->OnLogMessage(severity_, file_, line_, text);
  } else {
    // Last path component only, to keep lines readable.
    std::string_view path(file_);
    const size_t slash = path.find_last_of('/');
    if (slash != std::string_view::npos) {
      path.remove_prefix(slash + 1);
    }
    std::fprintf(stderr, "[%s %.*s:%d] %s\n", SeverityName(severity_),
                 static_cast<int>(path.size()), path.data(), line_,
                 text.c_str());
    std::fflush(stderr);
  }

  if (severity_ == LOG_FATAL) {
    std::abort();
  }
}

}  // namespace avbase::base::logging
