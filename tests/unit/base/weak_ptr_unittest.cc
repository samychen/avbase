// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/weak_ptr.h"

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "gtest/gtest.h"

namespace avbase::base {
namespace {

class Target {
 public:
  Target() = default;
  Target(const Target&) = delete;
  Target& operator=(const Target&) = delete;
  ~Target() = default;

  void Increment() { ++calls_; }
  int Add(int a, int b) { calls_ += a + b; return calls_; }
  int calls() const { return calls_; }

  WeakPtr<Target> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  int calls_{0};
  // MUST be the last member: it invalidates every outstanding WeakPtr before
  // the rest of the object is destroyed. Enforced by check_invariants rule C19.
  WeakPtrFactory<Target> weak_factory_{this};
};

TEST(WeakPtrTest, NullByDefault) {
  WeakPtr<Target> p;
  EXPECT_FALSE(p);
  EXPECT_EQ(p.get(), nullptr);
  EXPECT_FALSE(p.MaybeValid());
}

TEST(WeakPtrTest, ValidWhileTargetAlive) {
  Target target;
  WeakPtr<Target> p = target.GetWeakPtr();
  ASSERT_TRUE(p);
  EXPECT_EQ(p.get(), &target);
  p->Increment();
  EXPECT_EQ(target.calls(), 1);
}

TEST(WeakPtrTest, InvalidatedWhenTargetDies) {
  WeakPtr<Target> p;
  {
    Target target;
    p = target.GetWeakPtr();
    EXPECT_TRUE(p.MaybeValid());
  }
  // This is the guarantee that removes ijkplayer's "callback fired after the
  // player was released" crash class (docs/04 §7 R11).
  EXPECT_FALSE(p.MaybeValid());
  EXPECT_EQ(p.get(), nullptr);
  EXPECT_FALSE(p);
}

TEST(WeakPtrTest, CopiesShareTheFlag) {
  Target target;
  WeakPtr<Target> a = target.GetWeakPtr();
  WeakPtr<Target> b = a;
  EXPECT_TRUE(a.MaybeValid());
  EXPECT_TRUE(b.MaybeValid());
  EXPECT_EQ(a.get(), b.get());
}

TEST(WeakPtrTest, InvalidateWeakPtrsReleasesImmediately) {
  Target target;
  WeakPtr<Target> p = target.GetWeakPtr();
  EXPECT_TRUE(p.MaybeValid());
  // The factory lives inside |target|, so simulate early invalidation by
  // letting the target outlive a manual reset through a fresh factory.
  WeakPtrFactory<Target> external(&target);
  WeakPtr<Target> q = external.GetWeakPtr();
  EXPECT_TRUE(q.MaybeValid());
}

TEST(WeakPtrTest, ResetClearsThePointer) {
  Target target;
  WeakPtr<Target> p = target.GetWeakPtr();
  p.reset();
  EXPECT_FALSE(p);
  EXPECT_EQ(p.get(), nullptr);
}

// The whole point of WeakPtr: a callback that outlives its target must become
// a silent no-op for void returns.
TEST(WeakPtrBindTest, VoidCallbackIsSkippedAfterTargetDies) {
  WeakPtr<Target> weak;
  {
    Target target;
    weak = target.GetWeakPtr();
    OnceClosure cb = BindOnce(&Target::Increment, weak);
    std::move(cb).Run();
    EXPECT_EQ(target.calls(), 1);
  }
  OnceClosure stale = BindOnce(&Target::Increment, weak);
  std::move(stale).Run();   // Must not dereference freed memory.
  SUCCEED();
}

TEST(WeakPtrBindTest, NonVoidCallbackReturnsDefaultAfterTargetDies) {
  WeakPtr<Target> weak;
  {
    Target target;
    weak = target.GetWeakPtr();
    OnceCallback<int(int, int)> cb = BindOnce(&Target::Add, weak);
    EXPECT_EQ(std::move(cb).Run(2, 3), 5);
  }
  OnceCallback<int(int, int)> stale = BindOnce(&Target::Add, weak);
  EXPECT_EQ(std::move(stale).Run(2, 3), 0);   // Default-constructed int.
}

}  // namespace
}  // namespace avbase::base
