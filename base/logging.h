// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/logging.h` (BSD-3-Clause).

#ifndef AVBASE_BASE_LOGGING_H_
#define AVBASE_BASE_LOGGING_H_

#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#include "base/base_export.h"

namespace avbase {
namespace base {
namespace logging {

enum LogSeverity : int {
  LOG_VERBOSE = -1,   // Used by VLOG(n); n maps to -n.
  LOG_INFO = 0,
  LOG_WARNING = 1,
  LOG_ERROR = 2,
  LOG_FATAL = 3,      // Logs, then aborts.
};

// Embedders install a delegate to route avbase logs into their own logging
// system (Android logcat, spdlog, journald, ...). See docs/10 §11.1.
class AVBASE_BASE_EXPORT LoggingDelegate {
 public:
  virtual ~LoggingDelegate() = default;
  virtual void OnLogMessage(LogSeverity severity,
                            const char* file,
                            int line,
                            std::string_view message) = 0;
};

// Takes ownership. Pass nullptr to restore the default stderr logger.
// Thread-safe. Must not be called from inside a log statement.
AVBASE_BASE_EXPORT void SetDelegate(std::unique_ptr<LoggingDelegate> delegate);

AVBASE_BASE_EXPORT void SetMinLogLevel(LogSeverity severity);
AVBASE_BASE_EXPORT LogSeverity GetMinLogLevel();
AVBASE_BASE_EXPORT bool ShouldCreateLogMessage(LogSeverity severity);

// VLOG(n) is enabled when n <= GetMinVLogLevel(). Default: 0 (VLOG off).
AVBASE_BASE_EXPORT void SetMinVLogLevel(int level);
AVBASE_BASE_EXPORT int GetMinVLogLevel();
AVBASE_BASE_EXPORT bool ShouldCreateVerboseMessage(int verbosity);

class AVBASE_BASE_EXPORT LogMessage {
 public:
  LogMessage(const char* file, int line, LogSeverity severity);
  LogMessage(const LogMessage&) = delete;
  LogMessage& operator=(const LogMessage&) = delete;
  ~LogMessage();

  std::ostream& stream() { return stream_; }

 private:
  const char* file_;
  int line_;
  LogSeverity severity_;
  std::ostringstream stream_;
};

// Helper that makes `cond ? (void)0 : stream << ...` type-correct: the two
// ternary arms must share a type, so the logging arm is folded through
// operator& which returns void.
//
// Templated rather than taking std::ostream& because media/base/media_log.h
// reuses the same idiom with MediaLogRecord, which streams into an internal
// buffer and is not an ostream.
class LogMessageVoidify {
 public:
  template <typename T>
  void operator&(T&&) {}
};

}  // namespace logging
}  // namespace base
}  // namespace avbase

#define AVBASE_LOG_STREAM(severity)                                        \
  ::avbase::base::logging::LogMessage(__FILE__, __LINE__, severity).stream()

#define AVBASE_LOG_IS_ON(severity)                                         \
  ::avbase::base::logging::ShouldCreateLogMessage(                         \
      ::avbase::base::logging::LOG_##severity)

#define AVBASE_LAZY_STREAM(stream, condition)                              \
  !(condition) ? (void)0 : ::avbase::base::logging::LogMessageVoidify() & (stream)

#define LOG(severity) AVBASE_LAZY_STREAM(AVBASE_LOG_STREAM(                 \
    ::avbase::base::logging::LOG_##severity), AVBASE_LOG_IS_ON(severity))

#define LOG_IF(severity, condition) !(condition) ? (void)0 : LOG(severity)

#if defined(AVBASE_ENABLE_DCHECK)
#define DLOG(severity) LOG(severity)
#else
#define DLOG(severity) AVBASE_LAZY_STREAM(AVBASE_LOG_STREAM(                \
    ::avbase::base::logging::LOG_##severity), false)
#endif

#define VLOG_IS_ON(verbosity) \
  ::avbase::base::logging::ShouldCreateVerboseMessage(verbosity)

#define VLOG(verbosity)                                                   \
  AVBASE_LAZY_STREAM(AVBASE_LOG_STREAM(::avbase::base::logging::LOG_VERBOSE), \
                    VLOG_IS_ON(verbosity))

#if defined(AVBASE_ENABLE_DCHECK)
#define DVLOG(verbosity) VLOG(verbosity)
#else
#define DVLOG(verbosity) AVBASE_LAZY_STREAM(AVBASE_LOG_STREAM(              \
    ::avbase::base::logging::LOG_VERBOSE), false)
#endif

#endif  // AVBASE_BASE_LOGGING_H_
