// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/media_log.h` (BSD-3-Clause).
//
// Replaces ijkplayer's MPTRACE/av_log mix, which had no structure: a log line
// was a string with no machine-readable event type, so "what stage did the
// first frame take" could only be answered by grepping timestamps by hand.
// Events recorded here feed both base/logging and Player::DumpDiagnostics().

#ifndef AVBASE_MEDIA_BASE_MEDIA_LOG_H_
#define AVBASE_MEDIA_BASE_MEDIA_LOG_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "media/base/media_constants.h"
#include "media/media_export.h"

namespace avbase::media {

// One structured media event. The (type, properties) shape is what makes the
// first-frame breakdown and decoder-fallback history queryable instead of
// buried in prose.
struct AVBASE_MEDIA_EXPORT MediaLogEvent {
  enum class Level { kInfo, kWarning, kError };
  enum class Type {
    kOpenInput,
    kFindStreamInfo,
    kComponentOpen,
    kVideoDecoderChanged,
    kAudioDecoderChanged,
    kVideoDecoderFallback,
    kAudioVideoSync,
    kDownloadBandwidth,
    kBufferingStateChanged,
    kFrameDropped,
    kSeekStarted,
    kSeekCompleted,
    kError,
    kProperty,
  };

  Level level{Level::kInfo};
  Type type{Type::kProperty};
  std::map<std::string, std::string> properties;
  std::string message;
  base::TimeTicks wall_time;
};

AVBASE_MEDIA_EXPORT const char*
GetMediaLogEventTypeName(MediaLogEvent::Type type);
AVBASE_MEDIA_EXPORT const char*
GetMediaLogLevelName(MediaLogEvent::Level level);

class AVBASE_MEDIA_EXPORT MediaLog
    : public base::RefCountedThreadSafe<MediaLog> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  MediaLog();
  MediaLog(const MediaLog&) = delete;
  MediaLog& operator=(const MediaLog&) = delete;

  // Thread-safe. Forwards to base/logging at the mapped severity and keeps the
  // most recent kMediaLogEventCapacity events for DumpDiagnostics().
  void AddEvent(MediaLogEvent::Level level, MediaLogEvent::Type type,
                std::map<std::string, std::string> properties,
                std::string message);
  void AddEvent(MediaLogEvent::Level level, MediaLogEvent::Type type,
                std::string message);

  // Most recent events, oldest first.
  std::vector<MediaLogEvent> GetEvents() const;
  size_t event_count() const;
  void Clear();

  // Serialises the retained events as a JSON array for DumpDiagnostics().
  std::string ToJson() const;

 private:
  friend class base::RefCountedThreadSafe<MediaLog>;
  ~MediaLog();

  mutable base::Lock lock_;
  std::vector<MediaLogEvent> events_ GUARDED_BY(lock_);
};

// Streaming record helper, matching Chromium's MEDIA_LOG macro shape.
class AVBASE_MEDIA_EXPORT MediaLogRecord {
 public:
  MediaLogRecord(MediaLogEvent::Level level, MediaLogEvent::Type type,
                 MediaLog* log);
  MediaLogRecord(const MediaLogRecord&) = delete;
  MediaLogRecord& operator=(const MediaLogRecord&) = delete;
  ~MediaLogRecord();

  template <typename T>
  MediaLogRecord& operator<<(const T& value) {
    stream_ << value;
    return *this;
  }
  // Attaches a key/value pair to the event, e.g. `.With("codec", "hevc")`.
  MediaLogRecord& With(std::string key, std::string value);
  MediaLogRecord& With(std::string key, int64_t value);

 private:
  MediaLogEvent::Level level_;
  MediaLogEvent::Type type_;
  base::scoped_refptr<MediaLog> log_;  // Keeps the log alive across the record.
  std::map<std::string, std::string> properties_;
  std::ostringstream stream_;
};

// Discards the record when |log| is null, so call sites need no null check.
class AVBASE_MEDIA_EXPORT NullMediaLogRecord {
 public:
  template <typename T>
  NullMediaLogRecord& operator<<(const T&) {
    return *this;
  }
  NullMediaLogRecord& With(std::string, std::string) { return *this; }
  NullMediaLogRecord& With(std::string, int64_t) { return *this; }
};

}  // namespace avbase::media

#define MEDIA_LOG(level, log)                                       \
  !(log) ? (void)0                                                  \
         : ::avbase::base::logging::LogMessageVoidify() &           \
               ::avbase::media::MediaLogRecord(                     \
                   ::avbase::media::MediaLogEvent::Level::k##level, \
                   ::avbase::media::MediaLogEvent::Type::kProperty, (log))

#define MEDIA_LOG_EVENT(level, type, log)                           \
  !(log) ? (void)0                                                  \
         : ::avbase::base::logging::LogMessageVoidify() &           \
               ::avbase::media::MediaLogRecord(                     \
                   ::avbase::media::MediaLogEvent::Level::k##level, \
                   ::avbase::media::MediaLogEvent::Type::k##type, (log))

#endif  // AVBASE_MEDIA_BASE_MEDIA_LOG_H_
