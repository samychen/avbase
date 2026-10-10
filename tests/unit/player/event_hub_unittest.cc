// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The observer half of EventHub, plus the RAII contract Player::Subscription
// makes on top of it. Three things were wrong before this suite existed, and
// all three are use-after-frees rather than tidiness:
//
//   1. An observer removed -- and destroyed -- from inside another observer's
//      callback was still called: Dispatch had already copied its raw
//      PlayerObserver* into a local vector.
//   2. An observer removed from ANOTHER thread could be destroyed while a
//      dispatch was still inside it, so RemoveObserver now waits for the
//      in-flight dispatch before returning.
//   3. A Player::Subscription unsubscribed only when Reset() was called
//      explicitly; simply dropping one -- the documented RAII behaviour --
//      left the observer registered.
//
// (4) pins the observer id counter: reusing an id after a removal would let a
// stale Subscription's RemoveObserver hit a different entry.

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "base/functional/bind.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "player/event_hub.h"
#include "player/public/player.h"
#include "player/public/player_event.h"

namespace avbase {
namespace {

const base::TimeDelta kWaitTimeout = base::Seconds(5);

// Polls |pred| until it holds or the budget runs out. Used to wait for a
// dispatch to land without depending on internal synchronisation the public
// surface does not expose.
template <typename Predicate>
bool PumpUntil(Predicate pred, int max_rounds = 400) {
  for (int i = 0; i < max_rounds && !pred(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// Counts every event that reaches it. The counter is per instance, never
// static: the Subscription case below compares two live observers, and a shared
// counter would make that comparison vacuous.
class CountingObserver : public PlayerObserver {
 public:
  void OnEvent(const PlayerEvent&) override { calls.fetch_add(1); }
  std::atomic<int> calls{0};
};

// The victim's counter IS static, on purpose: the case that owns it reads the
// count after the observer itself has been destroyed.
class VictimObserver : public PlayerObserver {
 public:
  void OnEvent(const PlayerEvent&) override { calls.fetch_add(1); }
  static std::atomic<int> calls;
};
std::atomic<int> VictimObserver::calls{0};

// Removes and DESTROYS another observer on its first event -- exactly the
// interleaving the old snapshot-and-call loop could not survive, because the
// victim's address was already in the dispatch's local vector.
class ReaperObserver : public PlayerObserver {
 public:
  explicit ReaperObserver(EventHub* hub) : hub_(hub) {}

  void OnEvent(const PlayerEvent&) override {
    if (victim_ == nullptr) {
      return;
    }
    PlayerObserver* victim = victim_;
    const int id = victim_id_;
    victim_ = nullptr;
    hub_->RemoveObserver(id);
    delete victim;  // destroyed from INSIDE the dispatch
  }

  void SetVictim(PlayerObserver* victim, int id) {
    victim_ = victim;
    victim_id_ = id;
  }

 private:
  EventHub* hub_;
  PlayerObserver* victim_ = nullptr;
  int victim_id_ = 0;
};

// Parks inside OnEvent until released, so a dispatch can be held in flight
// while another thread tries to remove the observer.
class BlockingObserver : public PlayerObserver {
 public:
  void OnEvent(const PlayerEvent&) override {
    entered.Signal();
    release.Wait();
    finished.store(true);
  }

  static std::atomic<bool> finished;
  base::WaitableEvent entered;
  base::WaitableEvent release;
};
std::atomic<bool> BlockingObserver::finished{false};

// Posts one event and waits until the hub has finished dispatching it, using a
// closure queued behind it on the same FIFO thread as the sentinel.
bool PostAndAwaitDispatch(EventHub* hub) {
  base::WaitableEvent done;
  hub->Post(EventType::kCompleted, CompletedPayload{}, base::TimeDelta());
  hub->PostClosure(
      base::BindOnce([](base::WaitableEvent* e) { e->Signal(); }, &done));
  return done.TimedWait(kWaitTimeout);
}

TEST(EventHubObserverTest, ObserverDestroyedFromACallbackIsNotCalled) {
  VictimObserver::calls.store(0);
  EventHub hub;
  auto reaper = std::make_unique<ReaperObserver>(&hub);
  // The reaper owns the victim and deletes it mid-dispatch -- that is the whole
  // point of the case -- so the test keeps only a raw pointer. Giving it to a
  // unique_ptr as well would be a double free that masks the behaviour under
  // test.
  VictimObserver* raw_victim = new VictimObserver();

  hub.AddObserver(reaper.get());  // registered first, so it runs first
  const int victim_id = hub.AddObserver(raw_victim);
  reaper->SetVictim(raw_victim, victim_id);

  ASSERT_TRUE(PostAndAwaitDispatch(&hub))
      << "the dispatch never completed; the hub is wedged";
  // The victim was deleted before the loop reached its slot. Being called at
  // all means the loop used a stale raw pointer (and under ASan, a
  // heap-use-after-free aborts the run before this line).
  EXPECT_EQ(VictimObserver::calls.load(), 0)
      << "an observer destroyed inside a callback was still dispatched to";
}

TEST(EventHubObserverTest,
     RemovingFromAnotherThreadWaitsForTheInFlightDispatch) {
  BlockingObserver::finished.store(false);
  EventHub hub;
  auto observer = std::make_unique<BlockingObserver>();
  BlockingObserver* raw = observer.get();
  const int id = hub.AddObserver(raw);

  hub.Post(EventType::kCompleted, CompletedPayload{}, base::TimeDelta());
  ASSERT_TRUE(raw->entered.TimedWait(kWaitTimeout))
      << "the dispatch never entered the observer";

  std::atomic<bool> removal_beat_the_callback{false};
  std::thread remover([&, owned = std::move(observer)]() mutable {
    hub.RemoveObserver(id);
    // Past this point the observer is the remover's to destroy -- legal only
    // if the dispatch has stopped calling into it.
    removal_beat_the_callback.store(!BlockingObserver::finished.load());
    owned.reset();
  });

  // Let the remover get into RemoveObserver before the callback is allowed to
  // finish, so the assertion below really exercises the wait. Without it the
  // remover deletes the observer while OnEvent is still running.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  raw->release.Signal();
  remover.join();

  EXPECT_FALSE(removal_beat_the_callback.load())
      << "RemoveObserver returned while the dispatch was still inside the "
         "observer; the caller could have destroyed it mid-callback";
  EXPECT_TRUE(BlockingObserver::finished.load());
}

TEST(EventHubObserverTest, ObserverIdsAreNotReusedAfterRemoval) {
  EventHub hub;
  CountingObserver first;
  CountingObserver second;
  const int a = hub.AddObserver(&first);
  hub.RemoveObserver(a);
  const int b = hub.AddObserver(&second);
  // Reusing the id would let a stale Subscription's RemoveObserver name the
  // new observer -- removing the wrong one, or silently keeping its own.
  EXPECT_NE(a, b);
}

// The Subscription contract at player.h:130: the handle unsubscribes when it
// is destroyed, not only when Reset() is called by hand.
TEST(PlayerSubscriptionTest, DroppedSubscriptionStopsDeliveringEvents) {
  CountingObserver observer;
  CountingObserver sentinel;
  Player player;
  {
    Player::Subscription subscription = player.AddObserver(&observer);
    ASSERT_TRUE(subscription.active());
    // Reset() publishes StateChanged on the hub; nothing else is needed to
    // make the hub deliver something observable.
    player.Reset();
    ASSERT_TRUE(PumpUntil([&] { return observer.calls.load() > 0; }))
        << "the live subscription never received a single event";
  }
  const int delivered_while_alive = observer.calls.load();

  // A sentinel registered AFTER the other handle went out of scope is the
  // clock for the rest of the case: once it has seen an event, that same
  // dispatch has run the whole observer list, so the dead handle's silence is
  // meaningful rather than a race.
  Player::Subscription sentinel_subscription = player.AddObserver(&sentinel);
  player.Reset();
  ASSERT_TRUE(PumpUntil([&] { return sentinel.calls.load() > 0; }))
      << "the sentinel never saw an event; the hub is not delivering";

  EXPECT_EQ(observer.calls.load(), delivered_while_alive)
      << "an event reached an observer whose Subscription had been destroyed";
}

}  // namespace
}  // namespace avbase
