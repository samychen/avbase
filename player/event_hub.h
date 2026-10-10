// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_EVENT_HUB_H_
#define AVBASE_PLAYER_EVENT_HUB_H_

#include <stdint.h>

#include <utility>
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
  // observers_ helpers; the caller holds |lock_|.
  void EraseObserverLocked(int id) EXCLUSIVE_LOCKS_REQUIRED(lock_);
  bool ObserverPresentLocked(int id) const EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // One observer registration. Mutation discipline (this replaces the old
  // shared_ptr/alive-flag/dispatch-count protocol): observers_ is only ever
  // MUTATED on the hub's dispatch thread -- AddObserver under the lock from
  // any thread, RemoveObserver inline when already on the dispatch thread,
  // else via a task queued on the same FIFO thread. Dispatch therefore
  // snapshots raw (id, pointer) pairs and RE-CHECKS membership before every
  // call: a callback earlier in the list may have removed -- and destroyed --
  // a later observer (the reaper case), and the re-check is what makes a
  // raw-pointer snapshot safe. A cross-thread RemoveObserver queues its erase
  // BEHIND any dispatch already in flight (FIFO) and waits for it, so the
  // caller may destroy the observer the moment it returns.
  struct ObserverEntry {
    int id{0};
    base::raw_ptr<PlayerObserver> observer{nullptr};
  };

  base::Thread thread_;
  // Guards handler_, observers_, next_observer_id_ and next_sequence_. The
  // dispatch task takes it to snapshot the handler and observers, then runs
  // them without the lock -- a user callback must never execute while this
  // thread holds a lock the SetEventHandler path needs.
  base::Lock lock_;
  Player::EventHandler handler_;
  std::vector<ObserverEntry> observers_ GUARDED_BY(lock_);
  // Monotonic, never reused: an id belonging to a dead subscription must not
  // be able to name a later observer, or RemoveObserver would remove the wrong
  // one (or silently fail to remove its own).
  int next_observer_id_{1};
  int64_t next_sequence_{0};

  // Signalled by a sentinel task at the end of the queue; Shutdown waits on
  // it so that joining cannot discard queued events.
  base::WaitableEvent drained_;
};

}  // namespace avbase

#endif  // AVBASE_PLAYER_EVENT_HUB_H_
