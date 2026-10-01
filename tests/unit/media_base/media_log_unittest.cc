// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/media_log.h"

#include <string>
#include <thread>
#include <vector>

#include "base/logging.h"
#include "gtest/gtest.h"

namespace avbase::media {
namespace {

// Captures events instead of letting them reach base/logging, so the tests are
// quiet and can assert on what a host's LoggingDelegate would receive.
class RecordingDelegate : public base::logging::LoggingDelegate {
 public:
  // |file| and |line| are unnamed: this delegate records only what a host cares
  // about, and -Wunused-parameter (debug preset, -Werror) rejects named ones.
  void OnLogMessage(base::logging::LogSeverity severity, const char* /*file*/,
                    int /*line*/, std::string_view message) override {
    ++count;
    last = std::string(message);
    last_severity = severity;
  }
  int count{0};
  std::string last;
  base::logging::LogSeverity last_severity{base::logging::LOG_INFO};
};

class MediaLogTest : public ::testing::Test {
 protected:
  void SetUp() override { log_ = base::MakeRefCounted<MediaLog>(); }
  base::scoped_refptr<MediaLog> log_;
};

TEST_F(MediaLogTest, RecordsEventsWithLevelAndType) {
  log_->AddEvent(MediaLogEvent::Level::kWarning,
                 MediaLogEvent::Type::kVideoDecoderFallback, "h264 -> none");
  const auto events = log_->GetEvents();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].level, MediaLogEvent::Level::kWarning);
  EXPECT_EQ(events[0].type, MediaLogEvent::Type::kVideoDecoderFallback);
  EXPECT_EQ(events[0].message, "h264 -> none");
  EXPECT_FALSE(events[0].wall_time.is_null());
}

TEST_F(MediaLogTest, PropertiesAreRetained) {
  log_->AddEvent(MediaLogEvent::Level::kInfo,
                 MediaLogEvent::Type::kOpenInput,
                 {{"uri", "file:///tmp/a.mp4"}, {"elapsed_ms", "210"}},
                 "opened");
  const auto events = log_->GetEvents();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].properties.at("uri"), "file:///tmp/a.mp4");
  EXPECT_EQ(events[0].properties.at("elapsed_ms"), "210");
}

TEST_F(MediaLogTest, RingBufferCapsRetainedEvents) {
  for (int i = 0; i < static_cast<int>(kMediaLogEventCapacity) + 50; ++i) {
    log_->AddEvent(MediaLogEvent::Level::kInfo, MediaLogEvent::Type::kProperty,
                   std::to_string(i));
  }
  EXPECT_EQ(log_->event_count(), kMediaLogEventCapacity);
  // Oldest entries are dropped, newest retained.
  const auto events = log_->GetEvents();
  EXPECT_EQ(events.back().message,
            std::to_string(kMediaLogEventCapacity + 49));
}

TEST_F(MediaLogTest, ClearEmpties) {
  log_->AddEvent(MediaLogEvent::Level::kError, MediaLogEvent::Type::kError, "x");
  log_->Clear();
  EXPECT_EQ(log_->event_count(), 0u);
}

TEST_F(MediaLogTest, ToJsonIsWellFormed) {
  log_->AddEvent(MediaLogEvent::Level::kInfo,
                 MediaLogEvent::Type::kFindStreamInfo, {{"streams", "2"}},
                 "found \"two\" streams\nwith newline");
  const std::string json = log_->ToJson();
  EXPECT_NE(json.find("\"type\":\"find-stream-info\""), std::string::npos);
  EXPECT_NE(json.find("\\\"two\\\""), std::string::npos) << json;
  EXPECT_NE(json.find("\\n"), std::string::npos);
  EXPECT_EQ(json.front(), '[');
  EXPECT_EQ(json.back(), ']');
}

TEST_F(MediaLogTest, EventTypeNamesAreStable) {
  using T = MediaLogEvent::Type;
  EXPECT_STREQ(GetMediaLogEventTypeName(T::kOpenInput), "open-input");
  EXPECT_STREQ(GetMediaLogEventTypeName(T::kVideoDecoderFallback),
               "video-decoder-fallback");
  EXPECT_STREQ(GetMediaLogEventTypeName(T::kSeekCompleted), "seek-completed");
  EXPECT_STREQ(GetMediaLogLevelName(MediaLogEvent::Level::kWarning), "WARNING");
}

// The MEDIA_LOG macro must compile away to nothing when the log is null, so
// call sites on optional-log paths need no branch of their own.
TEST_F(MediaLogTest, MacroHandlesNullLog) {
  MediaLog* null_log = nullptr;
  MEDIA_LOG(Info, null_log) << "this must not crash";
  MEDIA_LOG_EVENT(Warning, VideoDecoderFallback, null_log) << "nor this";
  SUCCEED();
}

TEST_F(MediaLogTest, MacroRecordsThroughTheStreamingApi) {
  MEDIA_LOG_EVENT(Error, SeekCompleted, log_.get())
      << "seek to " << 1500 << "ms took " << 42 << "ms";
  const auto events = log_->GetEvents();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].level, MediaLogEvent::Level::kError);
  EXPECT_EQ(events[0].type, MediaLogEvent::Type::kSeekCompleted);
  EXPECT_EQ(events[0].message, "seek to 1500ms took 42ms");
}

TEST_F(MediaLogTest, ThreadSafeUnderConcurrentWriters) {
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([this, t]() {
      for (int i = 0; i < 200; ++i) {
        log_->AddEvent(MediaLogEvent::Level::kInfo,
                       MediaLogEvent::Type::kProperty,
                       {{"thread", std::to_string(t)}}, std::to_string(i));
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(log_->event_count(), kMediaLogEventCapacity);
}

}  // namespace
}  // namespace avbase::media
