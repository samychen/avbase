// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// BM_EventHub_PostVisit: the cost of one event travelling from a pipeline
// thread to the user's handler (docs/04 §8 budget: EventHub::Post() < 3 us).
//
// WHY A DRAINED BATCH AND NOT A ROUND TRIP. BM_PostTask_RoundTrip
// (bench_posttask.cc) times a *synchronous* hand-off: post, wait, repeat. The
// event hub is the opposite by design -- the S8 sequence of docs/04 §1 exists
// precisely so the pipeline never waits for user code. Timing one round trip
// per event would measure a shape the player never uses and would report two
// thread wake-ups (tens of microseconds) instead of the steady-state cost the
// budget is written against. So: post |iters| events, then post one sentinel
// closure and wait for it. The hub is FIFO, so the sentinel running proves
// every event posted before it has already run.
//
// WHAT THE NUMBER MEANS, STATED PLAINLY: it is max(enqueue, dispatch), not
// Post() alone. The single wait covers the whole batch, so whichever side is
// slower sets the per-op figure. That is the honest reading of a 3 us budget
// on an asynchronous path, and the more useful one -- a regression on EITHER
// side moves the number, where a pure enqueue timer would be blind to the
// dispatch half.
//
// Coverage limit, stated rather than hidden: the handler is a counter, so this
// times the hub's own plumbing (lock, queue, variant move, sequence number).
// It does not time a handler that does real work, and it does not exercise the
// observer re-check path (EventHub snapshots (id, pointer) pairs and re-checks
// membership before each call). Those are separate costs.

#include "bench.h"

#include <atomic>

#include "base/functional/bind.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "player/event_hub.h"
#include "player/public/player_event.h"

namespace {

using avbase::EventHub;
using avbase::EventPayload;
using avbase::EventType;
using avbase::PlayerEvent;
using avbase::StateChangedPayload;
using avbase::base::BindOnce;
using avbase::base::BindRepeating;
using avbase::base::TimeDelta;
using avbase::base::WaitableEvent;

// One hub for the whole process, not one per timed round. Two measurements
// say why, and the second is the one that matters:
//
//   * Thread create/join is *not* the reason you would expect. Built the other
//     way (a fixture on the stack, constructed and destroyed once per round)
//     it ran fine, but the first round came in at 2644 ns against 1978/1997 ns
//     for the next two -- a cold thread plus a cold scheduler, +32% on the
//     very round the adaptive scaler uses to pick the iteration count.
//   * Normal destruction is safe: that same stack-built version exited 0 on
//     every run. What is NOT safe is destroying an EventHub from *static
//     storage duration* (see SharedHub below).
//
// The handler counts visits so the dispatch side does work no optimiser can
// delete -- and it runs in another translation unit (event_hub.cc), which is
// what makes the call observable from here at all.
struct HubFixture {
  EventHub hub;
  std::atomic<uint64_t> visits{0};

  HubFixture() {
    hub.SetEventHandler(BindRepeating(
        [](std::atomic<uint64_t>* sink, const PlayerEvent&) {
          sink->fetch_add(1, std::memory_order_relaxed);
        },
        &visits));
  }
};

// Leaked on purpose, and the reason is a measured crash, not a style
// preference: with the fixture as a function-local static *object*, the
// process aborted at exit (SIGABRT, once SIGILL) on five of six runs, after
// every number had already been printed. ~EventHub joins the dispatch thread
// (event_hub.cc: Shutdown), and running that join from static destruction --
// after main has returned, with base's own process-wide state already being
// torn down -- is what blew up. A bench process is short-lived and exits
// immediately after measuring, so never destroying the hub costs nothing and
// removes an exit-time crash from the gate. Do not "fix" the leak by making
// the object owned again without re-running the binary several times.
HubFixture& SharedHub() {
  static HubFixture* fixture = new HubFixture();
  return *fixture;
}

AVBASE_BENCH(BM_EventHub_PostVisit) {
  HubFixture& fixture = SharedHub();
  // kStateChanged on purpose: the smallest alternative that still exercises
  // the variant (PreparedPayload carries a whole MediaInfo), and the event the
  // player posts most often -- one per PlayerState transition. The budget is
  // "one lock plus one variant move"; this is that move at its floor, not at
  // its worst case.
  const EventPayload payload = StateChangedPayload{};
  for (uint64_t i = 0; i < iters; ++i) {
    fixture.hub.Post(EventType::kStateChanged, payload, TimeDelta());
  }
  WaitableEvent drained(WaitableEvent::ResetPolicy::kManualReset,
                        WaitableEvent::InitialState::kNotSignaled);
  fixture.hub.PostClosure(
      BindOnce([](WaitableEvent* e) { e->Signal(); }, &drained));
  drained.Wait();
  avbase::bench::Sink(fixture.visits.load(std::memory_order_relaxed));
}

}  // namespace
