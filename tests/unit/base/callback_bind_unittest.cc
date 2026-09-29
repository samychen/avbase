// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/functional/bind.h"

#include <string>

#include "base/functional/callback_helpers.h"
#include "base/memory/ref_counted.h"
#include "gtest/gtest.h"

namespace ijkpp::base {
namespace {

int FreeAdd(int a, int b) { return a + b; }
void FreeSink(int* out, int v) { *out = v; }

class RefCountedTarget : public RefCountedThreadSafe<RefCountedTarget> {
 public:
  RefCountedTarget() = default;
  void Note(int v) { last_ = v; }
  int last() const { return last_; }

 private:
  friend class RefCountedThreadSafe<RefCountedTarget>;
  ~RefCountedTarget() = default;
  int last_{0};
};

struct Plain {
  void Note(int v) { last = v; }
  int Echo(int v) const { return v; }
  int last{0};
};

// ---- OnceCallback ----

TEST(OnceCallbackTest, NullByDefault) {
  OnceClosure cb;
  EXPECT_TRUE(cb.is_null());
  EXPECT_FALSE(static_cast<bool>(cb));
}

TEST(OnceCallbackTest, RunsALambda) {
  int out = 0;
  OnceClosure cb([&out]() { out = 7; });
  std::move(cb).Run();
  EXPECT_EQ(out, 7);
}

TEST(OnceCallbackTest, IsMoveOnly) {
  OnceClosure cb([]() {});
  OnceClosure moved = std::move(cb);
  EXPECT_TRUE(cb.is_null());
  EXPECT_FALSE(moved.is_null());
}

TEST(OnceCallbackTest, ReturnsAValue) {
  OnceCallback<int(int)> cb([](int v) { return v * 3; });
  EXPECT_EQ(std::move(cb).Run(5), 15);
}

TEST(OnceCallbackTest, ResetClears) {
  OnceClosure cb([]() {});
  cb.Reset();
  EXPECT_TRUE(cb.is_null());
}

// ---- BindOnce ----

TEST(BindOnceTest, BindsFreeFunctionArguments) {
  OnceCallback<int()> cb = BindOnce(&FreeAdd, 20, 22);
  EXPECT_EQ(std::move(cb).Run(), 42);
}

TEST(BindOnceTest, LeavesTrailingArgumentsUnbound) {
  OnceCallback<int(int)> cb = BindOnce(&FreeAdd, 40);
  EXPECT_EQ(std::move(cb).Run(2), 42);
}

TEST(BindOnceTest, BindsLambdaWithCapturedPointer) {
  int out = 0;
  OnceCallback<void(int)> cb = BindOnce(&FreeSink, &out);
  std::move(cb).Run(9);
  EXPECT_EQ(out, 9);
}

TEST(BindOnceTest, BindsMemberFunctionWithRawReceiver) {
  Plain plain;
  OnceClosure cb = BindOnce(&Plain::Note, &plain, 11);
  std::move(cb).Run();
  EXPECT_EQ(plain.last, 11);
}

TEST(BindOnceTest, BindsMemberFunctionWithScopedRefptrReceiver) {
  auto target = MakeRefCounted<RefCountedTarget>();
  {
    OnceClosure cb = BindOnce(&RefCountedTarget::Note, target, 5);
    std::move(cb).Run();
    EXPECT_EQ(target->last(), 5);
  }
  // The bound scoped_refptr kept the object alive for the callback's lifetime.
  OnceClosure cb2 = BindOnce(&RefCountedTarget::Note, target, 6);
  std::move(cb2).Run();
  EXPECT_EQ(target->last(), 6);
}

// A bound scoped_refptr must keep its target alive even if every other owner
// goes away first; media frames and decoders rely on this.
TEST(BindOnceTest, BoundScopedRefptrOutlivesOtherOwners) {
  OnceClosure cb;
  {
    auto target = MakeRefCounted<RefCountedTarget>();
    cb = BindOnce(&RefCountedTarget::Note, target, 99);
  }
  std::move(cb).Run();   // Target is still alive because the callback owns a ref.
  SUCCEED();
}

TEST(BindOnceTest, UnretainedPassesRawPointer) {
  Plain plain;
  OnceClosure cb = BindOnce(&Plain::Note, Unretained(&plain), 3);
  std::move(cb).Run();
  EXPECT_EQ(plain.last, 3);
}

TEST(BindOnceTest, ConstMemberFunction) {
  const Plain plain;
  OnceCallback<int()> cb = BindOnce(&Plain::Echo, &plain, 8);
  EXPECT_EQ(std::move(cb).Run(), 8);
}

// ---- RepeatingCallback ----

TEST(RepeatingCallbackTest, CanBeRunMultipleTimes) {
  int total = 0;
  RepeatingCallback<void(int)> cb =
      BindRepeating([](int* out, int v) { *out += v; }, &total);
  cb.Run(1);
  cb.Run(2);
  cb.Run(3);
  EXPECT_EQ(total, 6);
}

TEST(RepeatingCallbackTest, CopiesShareBoundState) {
  int total = 0;
  RepeatingCallback<void(int)> cb =
      BindRepeating([](int* out, int v) { *out += v; }, &total);
  RepeatingCallback<void(int)> copy = cb;
  copy.Run(4);
  EXPECT_EQ(total, 4);   // Same captured pointer, by design.
  cb.Run(1);
  EXPECT_EQ(total, 5);
}

TEST(RepeatingCallbackTest, ReturnsValues) {
  RepeatingCallback<int(int)> cb = BindRepeating(&FreeAdd, 1);
  EXPECT_EQ(cb.Run(1), 2);
  EXPECT_EQ(cb.Run(10), 11);
}

// ---- helpers ----

TEST(CallbackHelpersTest, DoNothingIsRunnableAndHarmless) {
  // Chromium's DoNothing() returns a *runnable* empty closure, not a null one,
  // so that `std::move(cb).Run()` on a "no completion action" is safe.
  OnceClosure cb = DoNothing();
  ASSERT_FALSE(cb.is_null());
  std::move(cb).Run();
  SUCCEED();
}

TEST(CallbackHelpersTest, ScopedClosureRunnerRunsOnScopeExit) {
  bool ran = false;
  {
    ScopedClosureRunner runner(BindOnce([](bool* b) { *b = true; }, &ran));
    EXPECT_FALSE(ran);
  }
  EXPECT_TRUE(ran);
}

TEST(CallbackHelpersTest, ScopedClosureRunnerRunsOnlyOnce) {
  int count = 0;
  ScopedClosureRunner runner(BindOnce([](int* c) { ++*c; }, &count));
  runner.Run();
  EXPECT_EQ(count, 1);
  runner.Reset();   // Scope exit must not run it again.
  EXPECT_EQ(count, 1);
}

TEST(CallbackHelpersTest, ScopedClosureRunnerMoveTransfers) {
  int count = 0;
  {
    ScopedClosureRunner a(BindOnce([](int* c) { ++*c; }, &count));
    ScopedClosureRunner b = std::move(a);
  }
  EXPECT_EQ(count, 1);
}

}  // namespace
}  // namespace ijkpp::base
