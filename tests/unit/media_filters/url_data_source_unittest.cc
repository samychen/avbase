// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/url_data_source.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "base/functional/bind.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "gtest/gtest.h"
#include "media/base/media_error.h"

namespace avbase::media {
namespace {
using media::ffmpeg::UrlDataSource;

constexpr auto kWaitTimeout = std::chrono::seconds(20);

std::string TestFilePath(const std::string& name) {
  return "file://" + std::string(AVBASE_TESTDATA_DIR) + "/" + name;
}

// The avio layer accepts file:// URLs, which makes the UrlDataSource
// testable offline while exercising the same avio open/read/seek/size path a
// network source takes. HTTP-specific behaviour (the retry interplay, hung
// connections) is covered by tools/check_weaknet.py against its own httpd.
class UrlDataSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(reader_thread_.Start());
    path_ = TestFilePath("audio_only.m4a");
    const std::string raw_path =
        std::string(AVBASE_TESTDATA_DIR) + "/audio_only.m4a";
    std::FILE* f = std::fopen(raw_path.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      file_bytes_.insert(file_bytes_.end(), buf, buf + n);
    }
    std::fclose(f);
    ASSERT_GT(file_bytes_.size(), 1000u);
  }

  base::Thread reader_thread_{"url-src-test"};
  std::string path_;
  std::vector<uint8_t> file_bytes_;
};

TEST_F(UrlDataSourceTest, FileSchemeServesBytesAndSize) {
  UrlDataSource source(path_);
  int64_t size = 0;
  ASSERT_TRUE(source.GetSize(&size));
  EXPECT_EQ(size, static_cast<int64_t>(file_bytes_.size()));
  EXPECT_TRUE(source.IsSeekable());
  EXPECT_FALSE(source.IsStreaming());

  std::vector<uint8_t> buf(1024);
  auto result = source.ReadBlocking(0, buf.size(), buf.data());
  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  EXPECT_EQ(*result, static_cast<int>(buf.size()));
  EXPECT_EQ(0, std::memcmp(buf.data(), file_bytes_.data(), buf.size()));

  // A seek per read: an arbitrary offset serves the right bytes.
  const int64_t offset = 4096;
  result = source.ReadBlocking(offset, 256, buf.data());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(0, std::memcmp(buf.data(), file_bytes_.data() + offset, 256));
}

TEST_F(UrlDataSourceTest, ReadAtEofReturnsZero) {
  UrlDataSource source(path_);
  int64_t size = 0;
  ASSERT_TRUE(source.GetSize(&size));
  std::vector<uint8_t> buf(16);
  auto result = source.ReadBlocking(size, 16, buf.data());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0);
}

TEST_F(UrlDataSourceTest, AsyncReadPostsResultNeverRunsInline) {
  UrlDataSource source(path_);

  base::WaitableEvent done;
  std::vector<uint8_t> buf(512);
  std::thread::id callback_tid;
  int64_t bytes_read = -1;
  source.Read(0, buf.size(), buf.data(), reader_thread_.task_runner(),
              base::BindOnce(
                  [](base::WaitableEvent* ev, std::thread::id* tid,
                     int64_t* out, media::DataSource::ReadResult result) {
                    *tid = std::this_thread::get_id();
                    if (result.has_value()) {
                      *out = *result;
                    }
                    ev->Signal();
                  },
                  &done, &callback_tid, &bytes_read));
  // Posted, never inline: the callback cannot have run before Read() returned
  // on THIS thread.
  EXPECT_FALSE(done.IsSignaled());
  const std::thread::id caller = std::this_thread::get_id();
  ASSERT_TRUE(done.TimedWait(base::Seconds(static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(kWaitTimeout).count()))))
      << "async read never completed";
  EXPECT_NE(callback_tid, caller);
  EXPECT_EQ(bytes_read, static_cast<int64_t>(buf.size()));
}

TEST_F(UrlDataSourceTest, AbortErrorsSubsequentReads) {
  UrlDataSource source(path_);
  source.Abort();
  std::vector<uint8_t> buf(64);
  auto result = source.ReadBlocking(0, buf.size(), buf.data());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kAborted);
}

TEST_F(UrlDataSourceTest, UnknownSchemeFailsReadsWithActionableError) {
  // avio has no handler for this scheme: open fails, and the error carries
  // the URI (docs/10's actionable-error contract).
  UrlDataSource source("this-scheme-does-not-exist://host/x");
  std::vector<uint8_t> buf(16);
  auto result = source.ReadBlocking(0, buf.size(), buf.data());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kSourceOpenFailed);
  EXPECT_NE(result.error().ToString().find("this-scheme-does-not-exist"),
            std::string::npos);
}

}  // namespace
}  // namespace avbase::media
