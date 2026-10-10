// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_EVENT_HUB_H_
#define AVBASE_PLAYER_EVENT_HUB_H_

#include <stdint.h>

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "player/public/player.h"
#include "player/public/player_event.h"
#include "player/public/player_export.h"

namespace avbase {

// The S8 sequence of docs/04 §1: one FIFO thread that runs the user's event
// handler and every PlayerObserver callback.
//
// WHY A DEDICATED THREAD. ijkplayer dispatched messages from inside its
// message loop while holding the player mutex, so a user callback that called
// back into the player deadlocked (docs/01 defect #9). Here the pipeline never
// runs user code: it posts an event, the hub serialises it, and the handler
// may safely call any Player method -- including Stop() and Reset()
// (Player class comment's reentry guarantee).
//
// LIFETIME. The hub is destroyed last in ~Player(), after the pipeline has
// stopped and the media sequence is joined, so every event handler observes a
// quiescent player. Flush() runs the already-queued events (the final
// StateChanged{to=kStopped} must still reach the host) and then joins.
class AVBASE_PLAYER_EXPORT EventHub {
 public:
  EventHub();
  EventHub(const EventHub&) = delete;
  EventHub& operator=(const EventHub&) = delete;
  ~EventHub();

  // Callable from any thread; takes effect for events posted after it.
  void SetEventHandler(Player::EventHandler handler);

  // Observers. |AddObserver| returns the id to pass to |RemoveObserver|
  // (Player::Subscription wraps exactly this pair).
  int AddObserver(PlayerObserver* observer);
  void RemoveObserver(int id);

  // Assigns the monotonic sequence number and enqueues for dispatch.
  void Post(EventType type, EventPayload payload, base::TimeDelta media_time);
  // Runs |closure| on the event sequence -- the sink for callbacks the
  // facade promised to deliver there (Player::SeekTo's SeekCB).
  void PostClosure(base::OnceClosure closure);
  void PostStateChanged(PlayerState from, PlayerState to,
                        base::TimeDelta media_time);
  void PostError(MediaError error, base::TimeDelta media_time);

  // Waits up to |timeout| for the events already enqueued to run (the final
  // StateChanged{to=kStopped} must still reach the host), then joins the
  // dispatch thread. Called once from ~Player(); idempotent. Bounded by
  // config.shutdown_timeout, per Δ15: leak the queue rather than hang the
  // caller.
  void Shutdown(base::TimeDelta timeout);

 private:
  void Dispatch(PlayerEvent event);
  // True when the caller is this hub's dispatch thread -- the one thread that
  // runs Dispatch, and therefore every handler and observer callback.
  bool OnDispatchThread() const;

  // One observer registration. Held by shared_ptr so that a Dispatch which has
  // already snapshotted the list keeps the ENTRY alive even when RemoveObserver
  // erases it meanwhile; |alive| is what tells such a snapshot to skip it, and
  // |observer| is only ever dereferenced while it is still true.
  struct ObserverEntry {
    int id{0};
    base::raw_ptr<PlayerObserver> observer{nullptr};
    bool alive{true};
  };

  base::Thread thread_;
  // Guards handler_, observers_, next_observer_id_, next_sequence_ and
  // dispatching_. The dispatch task takes it to snapshot the handler and
  // observers, then runs them without the lock -- a user callback must never
  // execute while this thread holds a lock the SetEventHandler path needs.
  base::Lock lock_;
  Player::EventHandler handler_;
  std::vector<std::shared_ptr<ObserverEntry>> observers_ GUARDED_BY(lock_);
  // Monotonic, never reused: an id belonging to a dead subscription must not
  // be able to name a later observer, or RemoveObserver would remove the wrong
  // one (or silently fail to remove its own).
  int next_observer_id_{1};
  int64_t next_sequence_{0};

  // ---- the in-flight-dispatch protocol ------------------------------------
  // Dispatch runs user code with no lock held, so an observer can be removed
  // -- and destroyed -- while a snapshot still holds its address.
  // RemoveObserver marks the entry dead at once (a snapshot checks the flag
  // before every call) and, unless it is itself on the dispatch thread, waits
  // here until the dispatch in flight has finished. From the dispatch thread
  // the wait is skipped: the callback being run IS that dispatch, and waiting
  // would deadlock. That case is still safe, because the entry is already dead
  // and the loop re-checks the flag.
  int dispatching_ GUARDED_BY(lock_) = 0;
  // Manual reset, and re-armed by Dispatch before it drops the lock: a waiter
  // that wakes between Signal and the next Reset still observes dispatching_.
  base::WaitableEvent drain_idle_;

  // Signalled by a sentinel task at the end of the queue; Shutdown waits on
  // it so that joining cannot discard queued events.
  base::WaitableEvent drained_;
};

}  // namespace avbase

#endif  // AVBASE_PLAYER_EVENT_HUB_H_
