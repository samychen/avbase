// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/throttled_data_source.h"

#include <array>
#include <chrono>
#include <cstring>
#include <thread>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "gtest/gtest.h"
#include "media/base/data_source.h"
#include "media/base/media_error.h"

namespace ijkpp::media::test {
namespace {

constexpr size_t kFileSize = 20000;

std::array<uint8_t, kFileSize> MakeFile() {
  std::array<uint8_t, kFileSize> bytes{};
  for (size_t i = 0; i < kFileSize; ++i) {
    bytes[i] = static_cast<uint8_t>(i & 0xff);
  }
  return bytes;
}

base::scoped_refptr<DataSource> MakeInner(
    const std::array<uint8_t, kFileSize>& bytes) {
  return base::MakeRefCounted<MemoryDataSource>(bytes.data(), bytes.size());
}

MediaError NetworkDied() {
  return MediaError::Of(ErrorCode::kNetworkUnreachable, "network died",
                        "injected by the test at a chosen offset",
                        "reconnect");
}

TEST(ThrottledDataSourceTest, RateZeroIsPassthrough) {
  auto bytes = MakeFile();
  ThrottledDataSource source(MakeInner(bytes), 0);
  std::array<uint8_t, kFileSize> out{};

  const auto start = std::chrono::steady_clock::now();
  auto result = source.ReadBlocking(0, kFileSize, out.data());
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), static_cast<int>(kFileSize));
  EXPECT_LT(elapsed, std::chrono::milliseconds(500));
  EXPECT_EQ(std::memcmp(out.data(), bytes.data(), kFileSize), 0);
  EXPECT_EQ(source.stalls(), 0);
}

TEST(ThrottledDataSourceTest, PacingCapsBytesPerSecond) {
  auto bytes = MakeFile();
  // 20 KB at 50 KB/s with a 2 KB burst: about (20-2)/50 = 360 ms of pacing.
  ThrottledDataSource source(MakeInner(bytes), 50 * 1000);
  source.set_max_burst_bytes(2000);
  std::array<uint8_t, kFileSize> out{};

  const auto start = std::chrono::steady_clock::now();
  auto result = source.ReadBlocking(0, kFileSize, out.data());
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), static_cast<int>(kFileSize));
  EXPECT_GE(elapsed, std::chrono::milliseconds(300))
      << "the throttle did not throttle";
  // Generous upper bound: this is a wall-clock test on a shared runner.
  EXPECT_LT(elapsed, std::chrono::seconds(5));
  EXPECT_GT(source.stalls(), 0);
  EXPECT_EQ(source.bytes_served(), static_cast<int64_t>(kFileSize));
}

TEST(ThrottledDataSourceTest, EofPropagatesAsZero) {
  auto bytes = MakeFile();
  ThrottledDataSource source(MakeInner(bytes), 0);
  std::array<uint8_t, 16> out{};

  auto result = source.ReadBlocking(kFileSize, 16, out.data());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 0);
}

TEST(ThrottledDataSourceTest, InjectedFailureIsOffsetGatedAndClearable) {
  auto bytes = MakeFile();
  ThrottledDataSource source(MakeInner(bytes), 0);
  std::array<uint8_t, 64> out{};

  source.FailFrom(10000, NetworkDied());

  auto before = source.ReadBlocking(0, 64, out.data());
  ASSERT_TRUE(before.has_value()) << "reads before the offset must survive";

  auto at = source.ReadBlocking(10000, 64, out.data());
  ASSERT_FALSE(at.has_value());
  EXPECT_EQ(at.error().code(), ErrorCode::kNetworkUnreachable);

  source.ClearFailure();
  auto after = source.ReadBlocking(10000, 64, out.data());
  ASSERT_TRUE(after.has_value()) << "ClearFailure() must rearm the source";
  EXPECT_EQ(after.value(), 64);
}

TEST(ThrottledDataSourceTest, AbortUnblocksAStalledRead) {
  auto bytes = MakeFile();
  // 1 KB/s over 20 KB would be 19+ seconds of pacing; the abort must cut it
  // far short.
  ThrottledDataSource source(MakeInner(bytes), 1000);
  std::array<uint8_t, kFileSize> out{};

  std::thread aborter([&source] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    source.Abort();
  });
  const auto start = std::chrono::steady_clock::now();
  auto result = source.ReadBlocking(0, kFileSize, out.data());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  aborter.join();

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kSourceReadFailed);
  EXPECT_LT(elapsed, std::chrono::seconds(2))
      << "Abort() must bound the stall (docs/04 §5.4 Δ15)";
}

TEST(ThrottledDataSourceTest, AsyncReadDeliversOnTheRunnerNeverInline) {
  auto bytes = MakeFile();
  ThrottledDataSource source(MakeInner(bytes), 0);
  base::test::TaskEnvironment env;
  std::array<uint8_t, 128> out{};

  bool ran = false;
  source.Read(0, out.size(), out.data(), env.GetMainThreadTaskRunnerRef(),
              base::BindOnce(
                  [](bool* ran, DataSource::ReadResult result) {
                    *ran = result.has_value();
                    if (result.has_value()) {
                      EXPECT_EQ(result.value(), 128);
                    } else {
                      ADD_FAILURE() << "async read failed";
                    }
                  },
                  &ran));
  EXPECT_FALSE(ran) << "callback ran inline, violating the DataSource "
                       "contract (media/base/data_source.h)";
  env.RunUntilIdle();
  EXPECT_TRUE(ran) << "callback never ran after pumping";
}

TEST(ThrottledDataSourceTest, DelegationMatchesTheInnerSource) {
  auto bytes = MakeFile();
  auto inner = MakeInner(bytes);
  ThrottledDataSource source(inner, 0);

  int64_t size = 0;
  EXPECT_TRUE(source.GetSize(&size));
  EXPECT_EQ(size, static_cast<int64_t>(kFileSize));
  EXPECT_FALSE(source.IsStreaming());
  EXPECT_TRUE(source.IsSeekable());
  source.SetBitrate(1234);  // Must not crash; inner ignores it too.
}

}  // namespace
}  // namespace ijkpp::media::test
