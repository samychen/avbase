// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/ffmpeg/log_bridge.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace avbase::platform::ffmpeg {
namespace {

// av_log's callback receives a partial line at a time and expects the callee to
// buffer until a newline arrives; without this, one FFmpeg message would be
// split across several avbase log lines.
thread_local std::string g_line_buffer;
thread_local int g_pending_level = AV_LOG_INFO;

void AvLogCallback(void* /*avcl*/, int level, const char* fmt, va_list args) {
  if (!base::logging::ShouldCreateLogMessage(MapAvLogLevel(level)) &&
      level != AV_LOG_FATAL) {
    return;
  }
  char buf[1024];
  // The format string is FFmpeg's, not ours: av_log_set_callback hands the
  // callback the same |fmt| the library would have printed itself, so it cannot
  // be a literal here. |args| matches it by construction (FFmpeg passes the two
  // together), which is precisely what -Wformat-nonliteral cannot see.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
  const int printed = vsnprintf(buf, sizeof(buf), fmt, args);
#pragma GCC diagnostic pop
  if (printed <= 0) {
    return;
  }
  if (g_line_buffer.empty()) {
    g_pending_level = level;
  }
  // vsnprintf returns the length it *would* have written, so a truncated line
  // has to be clamped to what |buf| actually holds.
  const size_t printable =
      std::min(static_cast<size_t>(printed), sizeof(buf) - 1);
  g_line_buffer.append(buf, printable);

  // Flush on newline, and strip FFmpeg's trailing newline so base/logging does
  // not add a blank line of its own.
  size_t pos;
  while ((pos = g_line_buffer.find('\n')) != std::string::npos) {
    std::string line = g_line_buffer.substr(0, pos);
    g_line_buffer.erase(0, pos + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    switch (MapAvLogLevel(g_pending_level)) {
    case base::logging::LOG_FATAL:
    case base::logging::LOG_ERROR:
      LOG(ERROR) << "[ffmpeg] " << line;
      break;
    case base::logging::LOG_WARNING:
      LOG(WARNING) << "[ffmpeg] " << line;
      break;
    default:
      VLOG(1) << "[ffmpeg] " << line;
      break;
    }
  }
}

std::once_flag g_install_once;

}  // namespace

base::logging::LogSeverity MapAvLogLevel(int av_level) {
  if (av_level <= AV_LOG_FATAL)
    return base::logging::LOG_FATAL;
  if (av_level <= AV_LOG_ERROR)
    return base::logging::LOG_ERROR;
  if (av_level <= AV_LOG_WARNING)
    return base::logging::LOG_WARNING;
  if (av_level <= AV_LOG_INFO)
    return base::logging::LOG_INFO;
  return base::logging::LOG_VERBOSE;
}

void InstallLogBridge() {
  std::call_once(g_install_once, []() {
    av_log_set_callback(&AvLogCallback);
    // FFmpeg is chatty at info level by default; avbase re-emits its own
    // structured milestones, so push FFmpeg down to warning and let VLOG(1)
    // surface the rest when explicitly asked for.
    av_log_set_level(AV_LOG_WARNING);
  });
}

std::string GetFFmpegVersionString() {
  return std::string(LIBAVFORMAT_IDENT) + " / " + LIBAVCODEC_IDENT;
}

}  // namespace avbase::platform::ffmpeg
