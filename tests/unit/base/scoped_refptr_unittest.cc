// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/scoped_refptr.h"

#include <atomic>
#include <thread>
#include <vector>

#include "base/memory/ref_counted.h"
#include "gtest/gtest.h"

namespace avbase::base {
namespace {

std::atomic<int> g_live_count{0};

class Counted : public RefCountedThreadSafe<Counted> {
 public:
  Counted() { g_live_count.fetch_add(1); }
  int value() const { return 42; }

 private:
  friend class RefCountedThreadSafe<Counted>;
  ~Counted() { g_live_count.fetch_sub(1); }
};

class ScopedRefptrTest : public ::testing::Test {
 protected:
  void SetUp() override { g_live_count.store(0); }
};

TEST_F(ScopedRefptrTest, NullByDefault) {
  scoped_refptr<Counted> p;
  EXPECT_FALSE(p);
  EXPECT_EQ(p.get(), nullptr);
}

TEST_F(ScopedRefptrTest, MakeRefCountedCreatesOneOwner) {
  {
    auto p = MakeRefCounted<Counted>();
    EXPECT_TRUE(p);
    EXPECT_EQ(g_live_count.load(), 1);
    EXPECT_EQ(p->value(), 42);
  }
  EXPECT_EQ(g_live_count.load(), 0);
}

TEST_F(ScopedRefptrTest, CopySharesOwnership) {
  auto a = MakeRefCounted<Counted>();
  {
    scoped_refptr<Counted> b = a;
    EXPECT_EQ(a.get(), b.get());
    EXPECT_EQ(g_live_count.load(), 1);
  }
  EXPECT_EQ(g_live_count.load(), 1);   // |a| still holds it.
}

TEST_F(ScopedRefptrTest, MoveTransfersOwnership) {
  auto a = MakeRefCounted<Counted>();
  Counted* raw = a.get();
  scoped_refptr<Counted> b = std::move(a);
  EXPECT_EQ(a.get(), nullptr);
  EXPECT_EQ(b.get(), raw);
  EXPECT_EQ(g_live_count.load(), 1);
}

TEST_F(ScopedRefptrTest, ResetReleases) {
  auto a = MakeRefCounted<Counted>();
  a.reset();
  EXPECT_FALSE(a);
  EXPECT_EQ(g_live_count.load(), 0);
}

TEST_F(ScopedRefptrTest, ReassignmentReleasesOld) {
  auto a = MakeRefCounted<Counted>();
  auto b = MakeRefCounted<Counted>();
  EXPECT_EQ(g_live_count.load(), 2);
  a = b;
  EXPECT_EQ(g_live_count.load(), 1);
  EXPECT_EQ(a.get(), b.get());
}

TEST_F(ScopedRefptrTest, Comparison) {
  auto a = MakeRefCounted<Counted>();
  auto b = MakeRefCounted<Counted>();
  EXPECT_EQ(a, a);
  EXPECT_NE(a, b);
  EXPECT_TRUE(a < b || b < a);
  scoped_refptr<Counted> null;
  EXPECT_EQ(null, nullptr);
  EXPECT_EQ(nullptr, null);
}

// RefCountedThreadSafe must survive concurrent AddRef/Release; media frames
// cross three sequences in normal playback.
TEST_F(ScopedRefptrTest, ThreadSafeRefCounting) {
  auto shared = MakeRefCounted<Counted>();
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([shared]() {
      for (int j = 0; j < 10000; ++j) {
        scoped_refptr<Counted> local = shared;
        EXPECT_EQ(local->value(), 42);
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(g_live_count.load(), 1);
}

}  // namespace
}  // namespace avbase::base
