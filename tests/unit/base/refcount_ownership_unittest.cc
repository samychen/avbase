// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round).
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
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

namespace avbase::base {
namespace {

// A ref-counted probe that reports its own liveness, so a test can assert both
// "the count is what I expect" and "the object was actually destroyed".
// The destructor is public on purpose: RefCounted<T>::Release() does
// `delete static_cast<const T*>(this)`, and a test type gains nothing from
// hiding it.
class Probe : public RefCounted<Probe> {
 public:
  Probe() = default;
  // The null check is the fix for four SEGFAULTs: tests that only inspect the
  // reference count never call WatchDestruction(), so destroyed_ is null and
  // the unconditional dereference crashed the process during teardown -- after
  // the test body had already passed, which is why ctest reported SEGFAULT
  // rather than a failed assertion.
  ~Probe() {
    if (destroyed_) {
      *destroyed_ = true;
    }
  }

  int ref_count() const { return RefCountedBase::ref_count(); }
  bool HasOneRef() const { return ref_count() == 1; }

  // Out-parameter rather than a member so the flag survives the object.
  void WatchDestruction(bool* destroyed) { destroyed_ = destroyed; }

 private:
  bool* destroyed_ = nullptr;
};

// The same shape, but thread-safe ref counting and a private constructor --
// i.e. the shape media::MediaLog has (public ctor, private dtor) inverted, and
// the constraint media::DataSource imposes. Only a derived type with a public
// constructor can be instantiated, which is why MakeRefCounted<DataSource>()
// does not compile while MakeRefCounted<MemoryDataSource>() does.
//
// Note the destructor is NOT declared `override`. RefCountedThreadSafe's
// destructor is deliberately non-virtual -- Release() does
// `delete static_cast<const T*>(this)` with T the complete derived type, so no
// dispatch is needed -- and base/memory/ref_counted.h spells out the rule:
// a class with no other virtual members declares `~Foo();`, and writing
// `~Foo() override;` is a compile error ("does not override").
// media::DataSource gets `virtual ~DataSource();` only because it DOES have
// virtual members.
class DerivedOnly : public RefCountedThreadSafe<DerivedOnly> {
 public:
  int ref_count() const { return RefCountedThreadSafeBase::ref_count(); }

 private:
  friend class RefCountedThreadSafe<DerivedOnly>;
  friend class Concrete;
  DerivedOnly() = default;
  ~DerivedOnly() = default;
};

class Concrete final : public DerivedOnly {
 public:
  Concrete() = default;
  // Not `override`: the base destructor is deliberately non-virtual.
  ~Concrete() = default;
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
  // The scoped_refptr overload: no .get() and no null check to reason about.
  auto alias = WrapRefCounted(owner);
  EXPECT_EQ(2, owner->ref_count());
  EXPECT_EQ(owner.get(), alias.get());
}

TEST(AdoptRefTest, TakesOverAnExistingReferenceWithoutAddingOne) {
  // This test asserted the wrong thing in its first version: it did
  // `AdoptRef(new Probe())` and expected a count of 1. But in avbase a freshly
  // newed ref-counted object has count 0, MakeRefCounted reaches 1 by way of
  // the *adding* constructor, and ~RefCountedBase DCHECKs the count is back to
  // 0. AdoptRef therefore means "take over a reference someone already holds",
  // and handing it a count-0 pointer is misuse -- the first Release() would
  // take the count to -1 and never delete. The correct handoff is release() on
  // one scoped_refptr and AdoptRef on the other, which is also the shape a
  // C-style factory returning a +1'd pointer has.
  bool destroyed = false;
  auto first = MakeRefCounted<Probe>();
  first->WatchDestruction(&destroyed);
  ASSERT_EQ(1, first->ref_count());

  // release() gives up the reference without calling Release().
  Probe* raw = first.release();
  ASSERT_NE(nullptr, raw);
  ASSERT_EQ(1, raw->ref_count());  // still referenced, now unowned

  {
    auto owner = AdoptRef(raw);
    // The whole point: still exactly one reference, not two. The pre-fix
    // spelling of AdoptRef added one here, which leaked the object.
    EXPECT_EQ(1, owner->ref_count());
    EXPECT_TRUE(owner->HasOneRef());
    EXPECT_FALSE(destroyed);
  }
  EXPECT_TRUE(destroyed);                // Release() ran exactly once.
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
}  // namespace avbase::base
