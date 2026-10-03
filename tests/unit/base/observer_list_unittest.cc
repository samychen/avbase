// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/observer_list.h"

#include <vector>

#include "gtest/gtest.h"

namespace avbase::base {
namespace {

struct Observer {
  virtual ~Observer() = default;
  virtual void OnEvent(int value) = 0;
};

struct RecordingObserver : Observer {
  void OnEvent(int value) override { events.push_back(value); }
  std::vector<int> events;
};

TEST(ObserverListTest, AddAndIterate) {
  ObserverList<Observer> list;
  RecordingObserver a, b;
  list.AddObserver(&a);
  list.AddObserver(&b);
  EXPECT_EQ(list.size(), 2u);

  for (ObserverList<Observer>::Iterator it(list); Observer* o = it.GetNext();) {
    o->OnEvent(1);
  }
  EXPECT_EQ(a.events, std::vector<int>({1}));
  EXPECT_EQ(b.events, std::vector<int>({1}));
}

TEST(ObserverListTest, EmptyList) {
  ObserverList<Observer> list;
  EXPECT_TRUE(list.empty());
  ObserverList<Observer>::Iterator it(list);
  EXPECT_EQ(it.GetNext(), nullptr);
}

TEST(ObserverListTest, HasObserver) {
  ObserverList<Observer> list;
  RecordingObserver a;
  EXPECT_FALSE(list.HasObserver(&a));
  list.AddObserver(&a);
  EXPECT_TRUE(list.HasObserver(&a));
}

// Removing an observer from inside its own notification is the canonical
// re-entrancy case; the list must not visit it again or crash.
TEST(ObserverListTest, RemoveDuringIterationIsSafe) {
  ObserverList<Observer> list;
  RecordingObserver a, b;
  list.AddObserver(&a);
  list.AddObserver(&b);

  for (ObserverList<Observer>::Iterator it(list); Observer* o = it.GetNext();) {
    o->OnEvent(5);
    if (o == &a) {
      list.RemoveObserver(&b);  // b must not be notified in this pass.
    }
  }
  EXPECT_EQ(a.events, std::vector<int>({5}));
  EXPECT_TRUE(b.events.empty());
  EXPECT_EQ(list.size(), 1u);
}

TEST(ObserverListTest, RemoveSelfDuringIterationIsSafe) {
  ObserverList<Observer> list;
  RecordingObserver a, b;
  list.AddObserver(&a);
  list.AddObserver(&b);

  for (ObserverList<Observer>::Iterator it(list); Observer* o = it.GetNext();) {
    if (o == &a) {
      list.RemoveObserver(&a);  // Self-removal.
    } else {
      o->OnEvent(7);
    }
  }
  EXPECT_TRUE(a.events.empty());
  EXPECT_EQ(b.events, std::vector<int>({7}));
  EXPECT_EQ(list.size(), 1u);
}

// Chromium's default ObserverListPolicy::kAll notifies observers added during
// iteration. avbase matches that, because player::EventHub dispatches
// StateChanged and a handler may legitimately subscribe to further events.
TEST(ObserverListTest, AddDuringIterationIsVisitedThisPass) {
  ObserverList<Observer> list;
  RecordingObserver a, late;
  list.AddObserver(&a);

  for (ObserverList<Observer>::Iterator it(list); Observer* o = it.GetNext();) {
    o->OnEvent(1);
    if (o == &a) {
      list.AddObserver(&late);
    }
  }
  EXPECT_EQ(a.events, std::vector<int>({1}));
  EXPECT_EQ(late.events, std::vector<int>({1}));
  EXPECT_EQ(list.size(), 2u);
}

// Removing during iteration must be deferred: erasing from the vector while an
// Iterator holds an index into it would skip or double-visit entries.
TEST(ObserverListTest, RemovalIsDeferredUntilIterationEnds) {
  ObserverList<Observer> list;
  RecordingObserver a, b, c;
  list.AddObserver(&a);
  list.AddObserver(&b);
  list.AddObserver(&c);

  for (ObserverList<Observer>::Iterator it(list); Observer* o = it.GetNext();) {
    if (o == &a) {
      list.RemoveObserver(&b);
      // |b| is already marked dead but still occupies its slot, so the index
      // the iterator holds stays valid and |c| is still reached.
      EXPECT_EQ(list.size(), 2u);
    } else {
      o->OnEvent(3);
    }
  }
  EXPECT_TRUE(a.events.empty());
  EXPECT_TRUE(b.events.empty());
  EXPECT_EQ(c.events, std::vector<int>({3}));
  EXPECT_EQ(list.size(), 2u);
}

TEST(ObserverListTest, NestedIteration) {
  ObserverList<Observer> list;
  RecordingObserver a;
  list.AddObserver(&a);

  for (ObserverList<Observer>::Iterator outer(list);
       Observer* o = outer.GetNext();) {
    for (ObserverList<Observer>::Iterator inner(list);
         Observer* i = inner.GetNext();) {
      i->OnEvent(2);
    }
    o->OnEvent(3);
  }
  EXPECT_EQ(a.events, (std::vector<int>{2, 3}));
}

TEST(ObserverListTest, Clear) {
  ObserverList<Observer> list;
  RecordingObserver a;
  list.AddObserver(&a);
  list.Clear();
  EXPECT_TRUE(list.empty());
  EXPECT_FALSE(list.HasObserver(&a));
}

}  // namespace
}  // namespace avbase::base
