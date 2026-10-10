// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/functional/bind.h"

#include <string>

#include "base/functional/callback_helpers.h"
#include "base/memory/ref_counted.h"
#include "gtest/gtest.h"

namespace avbase::base {
namespace {

int FreeAdd(int a, int b) {
  return a + b;
}
void FreeSink(int* out, int v) {
  *out = v;
}

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
  std::move(cb)
      .Run();  // Target is still alive because the callback owns a ref.
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
  EXPECT_EQ(total, 4);  // Same captured pointer, by design.
  cb.Run(1);
  EXPECT_EQ(total, 5);
}

TEST(RepeatingCallbackTest, ReturnsValues) {
  RepeatingCallback<int(int)> cb = BindRepeating(&FreeAdd, 1);
  EXPECT_EQ(cb.Run(1), 2);
  EXPECT_EQ(cb.Run(10), 11);
}

// A Repeating callback must survive every invocation: a by-value callee may
// move from its parameter, so stored value-type bound arguments reach it as
// lvalues and the BindState keeps a complete copy for the next run. Before the
// kIsOnce policy existed, the second Run() received an emptied string.
void TakeString(std::string sink, std::string* observed) {
  *observed = std::move(sink);
}

TEST(RepeatingCallbackTest, BoundByValueSurvivesRepeatedRuns) {
  RepeatingCallback<void(std::string*)> cb =
      BindRepeating(&TakeString, std::string("hello"));
  std::string first;
  cb.Run(&first);
  EXPECT_EQ(first, "hello");
  std::string second;
  cb.Run(&second);
  EXPECT_EQ(second, "hello") << "the bound value was moved out by the "
                                "first invocation (H4)";
}

// Move-counting proof that the stored value is never handed out as an rvalue:
// the callee takes its parameter by value, so a right-value hand-out would
// move-construct the parameter each run, while a left-value hand-out
// copy-constructs it (leaving the BindState intact).
struct MoveCounted {
  explicit MoveCounted(std::string v) : value(std::move(v)) {}
  MoveCounted(MoveCounted&& o) : value(std::move(o.value)) { ++moves; }
  MoveCounted(const MoveCounted& o) : value(o.value) { ++copies; }
  MoveCounted& operator=(MoveCounted&&) = delete;
  MoveCounted& operator=(const MoveCounted&) = delete;
  std::string value;
  static inline int moves = 0;
  static inline int copies = 0;
};

TEST(RepeatingCallbackTest, BoundByValueIsNotMovedBetweenRuns) {
  RepeatingCallback<void(std::string*)> cb = BindRepeating(
      [](MoveCounted bound, std::string* out) { *out = bound.value; },
      MoveCounted("payload"));
  // Reset after construction: binding itself moves the value into the
  // BindState, which is fine; the runs must not.
  MoveCounted::moves = 0;
  MoveCounted::copies = 0;
  for (int i = 0; i < 3; ++i) {
    std::string out;
    cb.Run(&out);
    EXPECT_EQ(out, "payload");
  }
  EXPECT_EQ(MoveCounted::moves, 0)
      << "a Repeating invocation moved from the stored bound argument";
}

// Once semantics keep the right to move: the single invocation still receives
// the complete bound value (moving it in is fine, losing it is not).
TEST(OnceCallbackTest, BoundByValueArrivesComplete) {
  OnceCallback<void(std::string*)> cb =
      BindOnce(&TakeString, std::string("hello"));
  std::string observed;
  std::move(cb).Run(&observed);
  EXPECT_EQ(observed, "hello");
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
  runner.Reset();  // Scope exit must not run it again.
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

// M13: Run()&& used to move a shared_ptr into a unique_ptr, which failed to
// compile the moment anyone instantiated the overload.
TEST(RepeatingCallbackTest, RvalueRunConsumesTheCallback) {
  RepeatingCallback<int(int)> cb = BindRepeating(&FreeAdd, 10);
  EXPECT_EQ(std::move(cb).Run(5), 15);
  EXPECT_FALSE(cb);  // Consumed: the invoker handle was moved out.
}

}  // namespace
}  // namespace avbase::base
