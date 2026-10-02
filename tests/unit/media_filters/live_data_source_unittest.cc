// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/live_data_source.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "gtest/gtest.h"
#include "media/base/media_error.h"

namespace avbase::media {
namespace {

constexpr auto kWait = std::chrono::seconds(10);

std::vector<uint8_t> PatternBytes(int64_t offset, size_t size) {
  std::vector<uint8_t> bytes(size);
  for (size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<uint8_t>((offset + i) & 0xff);
  }
  return bytes;
}

TEST(LiveDataSourceTest, ServesAppendedBytesWithTheStreamPattern) {
  LiveDataSource source;
  const std::vector<uint8_t> bytes = PatternBytes(0, 1000);
  source.Append(bytes.data(), bytes.size());
  uint8_t buf[100] = {0};
  const auto result = source.ReadBlocking(500, 100, buf);
  ASSERT_TRUE(result) << result.error().ToString();
  EXPECT_EQ(result.value(), 100);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(buf[i], static_cast<uint8_t>((500 + i) & 0xff));
  }
  EXPECT_EQ(source.appended_size(), 1000u);
}

TEST(LiveDataSourceTest, ReaderBlocksAtTheLiveEdgeUntilTheProducerAppends) {
  LiveDataSource source;
  source.Append(PatternBytes(0, 100).data(), 100);

  std::atomic<bool> finished{false};
  int served = 0;
  std::thread reader([&] {
    uint8_t buf[50];
    // Reading past the appended end blocks at the edge -- the defining
    // property; the producer appends mid-read below.
    auto r = source.ReadBlocking(100, 50, buf);
    finished.store(r.has_value());
    if (r.has_value()) {
      served = r.value();
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_FALSE(finished.load()) << "read past the edge did not block";

  source.Append(PatternBytes(100, 50).data(), 50);
  for (int i = 0; i < 2000 && !finished.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(finished.load());
  EXPECT_EQ(served, 50);
  reader.join();
}

TEST(LiveDataSourceTest, PartialReadReturnsWhatExistsWithoutWaiting) {
  LiveDataSource source;
  source.Append(PatternBytes(0, 40).data(), 40);
  uint8_t buf[100];
  // Request 100 with only 40 in the buffer and no Close(): a live consumer
  // gets what exists now -- waiting would stall on a growing stream.
  const auto result = source.ReadBlocking(0, 100, buf);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), 40);
}

TEST(LiveDataSourceTest, CloseTurnsTheEdgeIntoEof) {
  LiveDataSource source;
  source.Append(PatternBytes(0, 40).data(), 40);
  source.Close();
  uint8_t buf[100];
  const auto result = source.ReadBlocking(0, 100, buf);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value(), 40);   // partial serve up to the new EOF
  const auto eof = source.ReadBlocking(40, 100, buf);
  ASSERT_TRUE(eof);
  EXPECT_EQ(eof.value(), 0);       // past the end: EOF
}

TEST(LiveDataSourceTest, AbortBoundsTheEdgeWait) {
  LiveDataSource source;   // nothing appended, never closed
  std::atomic<bool> finished{false};
  std::thread reader([&] {
    uint8_t buf[10];
    auto r = source.ReadBlocking(0, 10, buf);
    finished.store(!r.has_value());   // abort must surface as an error
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const auto start = std::chrono::steady_clock::now();
  source.Abort();
  for (int i = 0; i < 2000 && !finished.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(finished.load()) << "Abort() did not bound the edge wait";
  EXPECT_LT(std::chrono::steady_clock::now() - start, kWait);
  reader.join();
}

TEST(LiveDataSourceTest, AsyncReadDeliversOnTheTaskRunner) {
  LiveDataSource source;
  source.Append(PatternBytes(0, 100).data(), 100);

  base::test::TaskEnvironment env;
  base::WaitableEvent done;
  DataSource::ReadResult observed = Err(ErrorCode::kAborted, "not run", {}, {});
  source.Read(10, 20, reinterpret_cast<uint8_t*>(&observed),
              env.GetMainThreadTaskRunnerRef(),
              base::BindOnce([](DataSource::ReadResult* out,
                                base::WaitableEvent* e,
                                DataSource::ReadResult r) {
                *out = std::move(r);
                e->Signal();
              },
                            &observed, &done));
  for (int i = 0; i < 2000 && !done.IsSignaled(); ++i) {
    env.RunUntilIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(done.IsSignaled());
  ASSERT_TRUE(observed) << observed.error().ToString();
  EXPECT_EQ(observed.value(), 20);
}

}  // namespace
}  // namespace avbase::media
