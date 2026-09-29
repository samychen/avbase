// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: DRAFT — NOT YET IN THE BUILD.
// Listed in tests/CMakeLists.txt under base_unittests, but written in an
// environment with no compiler, so it has never been compiled or run. It must
// not be counted in the 287/322 totals until a real build confirms it. Remove
// this banner the first time `ctest --preset no-ffmpeg` runs it green.
//
// What this suite pins down is the distinction between the three ownership
// verbs in base/memory/scoped_refptr.h. They are one word apart and a lifetime
// apart in consequence, and one of them (AdoptRef) was already wrong once: it
// added a reference instead of taking one over, which leaked the object and
// would have tripped ~RefCountedBase's DCHECK_EQ(count, 0) in a debug build.
// Nothing called it at the time, so the bug was latent; these tests exist so
// that the next such mistake is loud rather than latent.

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"

#include "gtest/gtest.h"

namespace ijkpp::base {
namespace {

// A ref-counted probe that reports its own liveness, so a test can assert both
// "the count is what I expect" and "the object was actually destroyed".
// The destructor is public on purpose: RefCounted<T>::Release() does
// `delete static_cast<const T*>(this)`, and a test type gains nothing from
// hiding it.
class Probe : public RefCounted<Probe> {
 public:
  Probe() = default;
  ~Probe() { *destroyed_ = true; }

  int ref_count() const { return RefCountedBase::ref_count(); }
  bool HasOneRef() const { return ref_count() == 1; }

  // Out-parameter rather than a member so the flag survives the object.
  void WatchDestruction(bool* destroyed) { destroyed_ = destroyed; }

 private:
  bool* destroyed_ = nullptr;
};

// The same shape, but thread-safe ref counting and a protected constructor --
// i.e. the shape media::DataSource has, which is the one M9's RetryDataSource
// and M18's CacheDataSource will be wrapped in.
class DerivedOnly : public RefCountedThreadSafe<DerivedOnly> {
 public:
  int ref_count() const { return RefCountedThreadSafeBase::ref_count(); }

 private:
  friend class RefCountedThreadSafe<DerivedOnly>;
  DerivedOnly() = default;
  ~DerivedOnly() = default;

  // Only a derived type with a public constructor can be instantiated, which
  // is exactly the constraint media::DataSource imposes and the reason
  // MakeRefCounted<DataSource>() does not compile while
  // MakeRefCounted<MemoryDataSource>() does.
  friend class Concrete;
};

class Concrete final : public DerivedOnly {
 public:
  Concrete() = default;
  ~Concrete() override = default;
};

TEST(WrapRefCountedTest, AddsExactlyOneReference) {
  auto owner = MakeRefCounted<Probe>();
  ASSERT_EQ(1, owner->ref_count());

  auto alias = WrapRefCounted(owner.get());
  EXPECT_EQ(2, owner->ref_count());
  EXPECT_EQ(owner.get(), alias.get());

  alias = nullptr;
  EXPECT_EQ(1, owner->ref_count());   // The alias released, the owner did not.
}

TEST(WrapRefCountedTest, NullIsSafeAndYieldsNullPtr) {
  Probe* nothing = nullptr;
  auto wrapped = WrapRefCounted(nothing);
  EXPECT_FALSE(wrapped);
  EXPECT_EQ(nullptr, wrapped.get());
}

TEST(WrapRefCountedTest, OverloadAcceptsAnExistingScopedRefptr) {
  auto owner = MakeRefCounted<Probe>();
  auto alias = WrapRefCounted(owner);   // No .get(), no null check to reason about.
  EXPECT_EQ(2, owner->ref_count());
  EXPECT_EQ(owner.get(), alias.get());
}

TEST(AdoptRefTest, DoesNotAddASecondReference) {
  bool destroyed = false;
  Probe* raw = new Probe();
  raw->WatchDestruction(&destroyed);
  ASSERT_EQ(0, raw->ref_count());

  {
    auto owner = AdoptRef(raw);
    // The whole point: one owner, one reference. The pre-fix spelling made this
    // 2 and leaked the object.
    EXPECT_EQ(1, owner->ref_count());
    EXPECT_TRUE(owner->HasOneRef());
    EXPECT_FALSE(destroyed);
  }
  EXPECT_TRUE(destroyed);               // Release() ran exactly once.
}

TEST(AdoptRefTest, NullIsSafe) {
  auto owner = AdoptRef(static_cast<Probe*>(nullptr));
  EXPECT_FALSE(owner);
}

TEST(MakeRefCountedTest, StartsAtOneReference) {
  auto p = MakeRefCounted<Probe>();
  EXPECT_EQ(1, p->ref_count());
  EXPECT_TRUE(p->HasOneRef());
}

TEST(MakeRefCountedTest, DestroysExactlyOnce) {
  bool destroyed = false;
  {
    auto p = MakeRefCounted<Probe>();
    p->WatchDestruction(&destroyed);
    EXPECT_FALSE(destroyed);
  }
  EXPECT_TRUE(destroyed);
}

// The decorator shape M9 needs: hold an inner scoped_refptr, and be able to
// hand out additional references to itself without either leaking or stealing.
TEST(OwnershipVocabularyTest, DecoratorCanShareItselfAndItsInner) {
  auto inner = MakeRefCounted<Probe>();
  auto decorator = MakeRefCounted<Concrete>();
  ASSERT_EQ(1, inner->ref_count());

  auto inner_alias = WrapRefCounted(inner.get());
  auto self_alias = WrapRefCounted(decorator.get());
  EXPECT_EQ(2, inner->ref_count());
  EXPECT_EQ(2, decorator->ref_count());

  inner_alias = nullptr;
  self_alias = nullptr;
  EXPECT_EQ(1, inner->ref_count());
  EXPECT_EQ(1, decorator->ref_count());
}

TEST(OwnershipVocabularyTest, ProtectedCtorTypeIsReachableThroughDerived) {
  // MakeRefCounted<Concrete>() works because Concrete's constructor is public,
  // even though DerivedOnly's is private. This is the pattern MemoryDataSource
  // and the decoder-factory test fakes already rely on.
  auto p = MakeRefCounted<Concrete>();
  EXPECT_EQ(1, p->ref_count());
}

}  // namespace
}  // namespace ijkpp::base
