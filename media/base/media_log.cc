// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/media_log.h"

#include <cstdio>
#include <utility>

#include "base/logging.h"
#include "base/time/default_tick_clock.h"

namespace avbase::media {
namespace {

std::string EscapeJson(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out += c;
      }
    }
  }
  return out;
}

}  // namespace

const char* GetMediaLogEventTypeName(MediaLogEvent::Type type) {
  using T = MediaLogEvent::Type;
  switch (type) {
  case T::kOpenInput:
    return "open-input";
  case T::kFindStreamInfo:
    return "find-stream-info";
  case T::kComponentOpen:
    return "component-open";
  case T::kVideoDecoderChanged:
    return "video-decoder-changed";
  case T::kAudioDecoderChanged:
    return "audio-decoder-changed";
  case T::kVideoDecoderFallback:
    return "video-decoder-fallback";
  case T::kAudioVideoSync:
    return "audio-video-sync";
  case T::kDownloadBandwidth:
    return "download-bandwidth";
  case T::kBufferingStateChanged:
    return "buffering-state-changed";
  case T::kFrameDropped:
    return "frame-dropped";
  case T::kSeekStarted:
    return "seek-started";
  case T::kSeekCompleted:
    return "seek-completed";
  case T::kError:
    return "error";
  case T::kProperty:
    return "property";
  }
  return "invalid";
}

const char* GetMediaLogLevelName(MediaLogEvent::Level level) {
  using L = MediaLogEvent::Level;
  switch (level) {
  case L::kInfo:
    return "INFO";
  case L::kWarning:
    return "WARNING";
  case L::kError:
    return "ERROR";
  }
  return "INVALID";
}

MediaLog::MediaLog() = default;
MediaLog::~MediaLog() = default;

void MediaLog::AddEvent(MediaLogEvent::Level level, MediaLogEvent::Type type,
                        std::map<std::string, std::string> properties,
                        std::string message) {
  MediaLogEvent event;
  event.level = level;
  event.type = type;
  event.properties = std::move(properties);
  event.message = std::move(message);
  event.wall_time = base::DefaultTickClock::GetInstance()->NowTicks();

  // Mirror into base/logging so a host that installed a LoggingDelegate sees
  // media events alongside everything else (behaviour difference Δ22).
  std::string line = std::string(GetMediaLogEventTypeName(type));
  if (!event.message.empty()) {
    line += ": " + event.message;
  }
  for (const auto& [key, value] : event.properties) {
    line += " " + key + "=" + value;
  }
  switch (level) {
  case MediaLogEvent::Level::kError:
    LOG(ERROR) << "[media] " << line;
    break;
  case MediaLogEvent::Level::kWarning:
    LOG(WARNING) << "[media] " << line;
    break;
  case MediaLogEvent::Level::kInfo:
    LOG(INFO) << "[media] " << line;
    break;
  }

  base::AutoLock scoped(lock_);
  events_.push_back(std::move(event));
  if (events_.size() > kMediaLogEventCapacity) {
    events_.erase(events_.begin(),
                  events_.begin() + static_cast<long>(events_.size() -
                                                      kMediaLogEventCapacity));
  }
}

void MediaLog::AddEvent(MediaLogEvent::Level level, MediaLogEvent::Type type,
                        std::string message) {
  AddEvent(level, type, {}, std::move(message));
}

std::vector<MediaLogEvent> MediaLog::GetEvents() const {
  base::AutoLock scoped(lock_);
  return events_;
}

size_t MediaLog::event_count() const {
  base::AutoLock scoped(lock_);
  return events_.size();
}

void MediaLog::Clear() {
  base::AutoLock scoped(lock_);
  events_.clear();
}

std::string MediaLog::ToJson() const {
  std::vector<MediaLogEvent> events = GetEvents();
  std::string out = "[";
  for (size_t i = 0; i < events.size(); ++i) {
    const MediaLogEvent& e = events[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"level\":\"";
    out += GetMediaLogLevelName(e.level);
    out += "\",\"type\":\"";
    out += GetMediaLogEventTypeName(e.type);
    out += "\",\"message\":\"" + EscapeJson(e.message) + "\",\"props\":{";
    bool first = true;
    for (const auto& [key, value] : e.properties) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += "\"" + EscapeJson(key) + "\":\"" + EscapeJson(value) + "\"";
    }
    out += "}}";
  }
  out += "]";
  return out;
}

MediaLogRecord::MediaLogRecord(MediaLogEvent::Level level,
                               MediaLogEvent::Type type, MediaLog* log)
    : level_(level), type_(type), log_(log) {}

MediaLogRecord::~MediaLogRecord() {
  if (log_) {
    log_->AddEvent(level_, type_, std::move(properties_), stream_.str());
  }
}

MediaLogRecord& MediaLogRecord::With(std::string key, std::string value) {
  properties_[std::move(key)] = std::move(value);
  return *this;
}

MediaLogRecord& MediaLogRecord::With(std::string key, int64_t value) {
  properties_[std::move(key)] = std::to_string(value);
  return *this;
}

}  // namespace avbase::media
