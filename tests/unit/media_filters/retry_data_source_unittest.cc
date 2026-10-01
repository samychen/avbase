// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/retry_data_source.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/media_error.h"

namespace ijkpp::media {
namespace {

// A source that fails the first |failures_left_| reads at or beyond
// |fail_from_| (RetryDataSource re-issues the SAME request, so a plain
// counter on every read also works), then serves from a 1 KB pattern.
class FlakyDataSource final : public DataSource {
 public:
  explicit FlakyDataSource(int failures_left)
      : failures_left_(failures_left) {}

  void FailForever() { failures_left_ = 1 << 30; }

  int served_ok() const { return served_ok_.load(); }

  void SetHost(Host*) override {}
  void Read(int64_t offset, size_t size, uint8_t* data,
            base::scoped_refptr<base::TaskRunner> runner,
            ReadCB cb) override {
    runner->PostTask(
        FROM_HERE, base::BindOnce(std::move(cb), ReadBlocking(offset, size,
                                                              data)));
  }
  ReadResult ReadBlocking(int64_t offset, size_t size,
                          uint8_t* data) override {
    if (failures_left_.fetch_sub(1) > 0) {
      return Err(ErrorCode::kNetworkUnreachable, "flaky source failed", {},
                 "test fault");
    }
    for (size_t i = 0; i < size; ++i) {
      data[i] = static_cast<uint8_t>((offset + i) & 0xff);
    }
    served_ok_.fetch_add(1);
    return static_cast<int>(size);
  }
  void Abort() override { aborted_ = true; }
  bool GetSize(int64_t* out) override {
    *out = 4096;
    return true;
  }
  bool IsStreaming() const override { return false; }
  void SetBitrate(int) override {}
  bool IsSeekable() const override { return true; }

  std::atomic<int> served_ok_{0};

 private:
  std::atomic<int> failures_left_;
  std::atomic<bool> aborted_{false};
};

TEST(RetryDataSourceTest, RecoversWhenTheInnerSourceRecovers) {
  auto inner = base::MakeRefCounted<FlakyDataSource>(1);
  RetryDataSource::Config config;
  config.max_retries = 3;
  config.retry_delay = base::Milliseconds(1);
  RetryDataSource source(inner, config);
  uint8_t buf[16] = {0};
  const auto result = source.ReadBlocking(100, 16, buf);
  ASSERT_TRUE(result) << result.error().ToString();
  EXPECT_EQ(result.value(), 16);
  EXPECT_EQ(buf[0], 100);   // the pattern carries the offset
  EXPECT_EQ(inner->served_ok(), 1);
}

TEST(RetryDataSourceTest, GivesUpAfterTheRetryBudget) {
  auto inner = base::MakeRefCounted<FlakyDataSource>(0);
  inner->FailForever();
  RetryDataSource::Config config;
  config.max_retries = 2;
  config.retry_delay = base::Milliseconds(1);
  RetryDataSource source(inner, config);
  uint8_t buf[8];
  const auto result = source.ReadBlocking(0, 8, buf);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::kNetworkUnreachable);
  EXPECT_EQ(inner->served_ok(), 0);
}

TEST(RetryDataSourceTest, AsyncReadDeliversOnTheTaskRunner) {
  auto inner = base::MakeRefCounted<FlakyDataSource>(1);
  RetryDataSource::Config config;
  config.max_retries = 2;
  config.retry_delay = base::Milliseconds(1);
  RetryDataSource source(inner, config);

  base::test::TaskEnvironment env;
  base::WaitableEvent done;
  DataSource::ReadResult observed = Err(ErrorCode::kAborted, "not run", {}, {});
  source.Read(7, 4, reinterpret_cast<uint8_t*>(&observed),
              env.GetMainThreadTaskRunnerRef(),
              base::BindOnce([](DataSource::ReadResult* out,
                                base::WaitableEvent* e,
                                DataSource::ReadResult r) {
                *out = std::move(r);
                e->Signal();
              },
                            &observed, &done));
  // The worker retries on its own thread; the result lands on this task
  // runner, which must be pumped for the callback to run at all.
  for (int i = 0; i < 2000 && !done.IsSignaled(); ++i) {
    env.RunUntilIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(done.IsSignaled());
  ASSERT_TRUE(observed) << observed.error().ToString();
  EXPECT_EQ(observed.value(), 4);
}

TEST(RetryDataSourceTest, AbortEndsABlockedRetryPromptly) {
  auto inner = base::MakeRefCounted<FlakyDataSource>(0);
  inner->FailForever();
  RetryDataSource::Config config;
  config.max_retries = 1000;             // effectively forever
  config.retry_delay = base::Seconds(1);  // long slices, abort must cut them
  RetryDataSource source(inner, config);

  std::atomic<bool> finished{false};
  DataSource::ReadResult observed = Err(ErrorCode::kAborted, "not run", {}, {});
  std::thread reader([&] {
    uint8_t buf[8];
    observed = source.ReadBlocking(0, 8, buf);
    finished.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  source.Abort();
  for (int i = 0; i < 200 && !finished.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(finished.load()) << "Abort() did not bound the blocked read";
  EXPECT_FALSE(observed.has_value());
  reader.join();
}

}  // namespace
}  // namespace ijkpp::media
