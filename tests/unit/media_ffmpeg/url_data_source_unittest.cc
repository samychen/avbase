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
#include "base/task/task_queue.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/media_error.h"

namespace avbase::media {
namespace {
using media::ffmpeg::UrlDataSource;

constexpr auto kWaitTimeout = std::chrono::seconds(20);

// The same budget is far too generous for the "the worker posted yet?" wait:
// that is a 512-byte file read, so a regression should fail the test quickly
// rather than after twenty seconds.
constexpr auto kPostWaitTimeout = std::chrono::seconds(5);

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
  // The contract is about WHICH THREAD the result arrives on, so that is what
  // this asserts: the runner the caller named, never the caller's own thread.
  //
  // This test used to also assert EXPECT_FALSE(done.IsSignaled()) here. That
  // looked like the same claim and was not: it is a statement about scheduling,
  // and on a loaded -j8 run the reader thread finishes a 512-byte file read and
  // posts back before this thread gets to look, so it failed without any
  // contract being broken. Asserting the callback's thread is deterministic;
  // asserting the clock is not. The "not inline" half moved to the TaskQueue
  // test below, where it is a fact rather than a race.
  const std::thread::id caller = std::this_thread::get_id();
  ASSERT_TRUE(done.TimedWait(base::Seconds(static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(kWaitTimeout).count()))))
      << "async read never completed";
  EXPECT_EQ(callback_tid, reader_thread_.GetThreadId());
  EXPECT_NE(callback_tid, caller);
  EXPECT_EQ(bytes_read, static_cast<int64_t>(buf.size()));
}

// The deterministic half of the same contract. A TaskQueue runs nothing until
// it is pumped, so "the callback had not run by the time Read() returned" is
// observable instead of racy: the result is still PENDING, and only
// RunAllReadyTasks() can run it. The wait below is for the worker to post --
// a bounded wait for something that must happen, which is the benign shape;
// the negative assertion that flaked is the one that is gone.
TEST_F(UrlDataSourceTest, AsyncReadQueuesTheResultInsteadOfRunningIt) {
  UrlDataSource source(path_);
  auto queue = base::MakeRefCounted<base::TaskQueue>();

  std::vector<uint8_t> buf(512);
  int64_t bytes_read = -1;
  source.Read(0, buf.size(), buf.data(), queue,
              base::BindOnce(
                  [](int64_t* out, media::DataSource::ReadResult result) {
                    if (result.has_value()) {
                      *out = *result;
                    }
                  },
                  &bytes_read));

  const auto deadline = std::chrono::steady_clock::now() + kPostWaitTimeout;
  while (queue->GetPendingTaskCount() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(queue->GetPendingTaskCount(), 1u)
      << "the async read never posted its result to the given runner";
  EXPECT_EQ(bytes_read, -1)
      << "the callback ran without the queue being pumped";

  queue->RunAllReadyTasks(base::TimeTicks::Now());
  EXPECT_EQ(bytes_read, static_cast<int64_t>(buf.size()));
  EXPECT_EQ(queue->GetPendingTaskCount(), 0u);
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
