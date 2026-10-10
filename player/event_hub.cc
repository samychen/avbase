// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/event_hub.h"

#include <cstddef>
#include <memory>
#include <thread>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"

namespace avbase {

EventHub::EventHub() : thread_("avbase-event") {
  thread_.Start();
}

EventHub::~EventHub() {
  Shutdown(base::Milliseconds(500));
}

void EventHub::SetEventHandler(Player::EventHandler handler) {
  base::AutoLock scoped(lock_);
  handler_ = std::move(handler);
}

int EventHub::AddObserver(PlayerObserver* observer) {
  base::AutoLock scoped(lock_);
  const int id = next_observer_id_++;
  observers_.push_back(ObserverEntry{id, observer});
  return id;
}

void EventHub::EraseObserverLocked(int id) {
  for (auto it = observers_.begin(); it != observers_.end(); ++it) {
    if (it->id == id) {
      observers_.erase(it);
      return;
    }
  }
}

bool EventHub::ObserverPresentLocked(int id) const {
  for (const ObserverEntry& entry : observers_) {
    if (entry.id == id) {
      return true;
    }
  }
  return false;
}

void EventHub::RemoveObserver(int id) {
  // On the dispatch thread: inline, synchronous erase. The Dispatch loop
  // re-checks membership before every call, so an entry erased -- and its
  // observer destroyed -- by a callback earlier in the same dispatch is
  // skipped, never dereferenced. (Also the fallback for a stopped thread:
  // with no dispatch thread there is no in-flight dispatch to wait for.)
  if (OnDispatchThread() || !thread_.IsRunning()) {
    base::AutoLock scoped(lock_);
    EraseObserverLocked(id);
    return;
  }
  // Elsewhere: the erase must run AFTER any dispatch already in flight (the
  // caller may destroy the observer the moment this returns), so it is queued
  // on the hub's own FIFO thread -- the only thread Dispatch runs on -- and
  // this call waits for it. The old shared_ptr/alive/dispatching_/drain_idle_
  // protocol existed to answer exactly this question; FIFO ordering answers
  // it with no protocol at all. |erased| is stack-local and safe to name in
  // the task because this call does not return before the task signals it.
  base::WaitableEvent erased;
  thread_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](EventHub* self, int observer_id, base::WaitableEvent* done) {
            {
              base::AutoLock scoped(self->lock_);
              self->EraseObserverLocked(observer_id);
            }
            done->Signal();
          },
          base::Unretained(this), id, &erased));
  erased.Wait();
}

bool EventHub::OnDispatchThread() const {
  return std::this_thread::get_id() == thread_.GetThreadId();
}

void EventHub::Post(EventType type, EventPayload payload,
                    base::TimeDelta media_time) {
  PlayerEvent event;
  event.type = type;
  event.payload = std::move(payload);
  event.wall_time = base::TimeTicks::Now();
  event.media_time = media_time;
  {
    base::AutoLock scoped(lock_);
    event.sequence_number = ++next_sequence_;
  }
  thread_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&EventHub::Dispatch, base::Unretained(this),
                                std::move(event)));
}

void EventHub::PostClosure(base::OnceClosure closure) {
  thread_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce([](base::OnceClosure task) { std::move(task).Run(); },
                     std::move(closure)));
}

void EventHub::PostStateChanged(PlayerState from, PlayerState to,
                                base::TimeDelta media_time) {
  StateChangedPayload payload;
  payload.from = from;
  payload.to = to;
  Post(EventType::kStateChanged, std::move(payload), media_time);
}

void EventHub::PostError(MediaError error, base::TimeDelta media_time) {
  ErrorPayload payload;
  payload.error = std::move(error);
  Post(EventType::kError, std::move(payload), media_time);
}

void EventHub::Dispatch(PlayerEvent event) {
  Player::EventHandler handler;
  std::vector<std::pair<int, PlayerObserver*>> observers;
  {
    base::AutoLock scoped(lock_);
    handler = handler_;
    observers.reserve(observers_.size());
    for (const ObserverEntry& entry : observers_) {
      observers.emplace_back(entry.id, entry.observer);
    }
  }
  // The handler runs with no lock held: it may call any Player method, which
  // may itself post more events or try to take this same lock.
  if (handler) {
    handler.Run(event);
  }
  for (const auto& entry : observers) {
    // The observer behind this snapshot slot may have been removed -- and
    // destroyed -- by the handler above, by an earlier observer in this same
    // loop (the reaper case), or by an earlier loop iteration's callback
    // removing a later registration. Re-check membership before every call;
    // nothing can remove it BETWEEN this check and the call, because the only
    // threads that mutate observers_ are this one (inline) and erase tasks
    // queued behind this very dispatch on the same FIFO thread.
    bool present;
    {
      base::AutoLock scoped(lock_);
      present = ObserverPresentLocked(entry.first);
    }
    if (present) {
      entry.second->OnEvent(event);
    }
  }
}

void EventHub::Shutdown(base::TimeDelta timeout) {
  if (!thread_.IsRunning()) {
    return;
  }
  if (!drained_.IsSignaled()) {
    thread_.task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce([](base::WaitableEvent* e) { e->Signal(); }, &drained_));
    drained_.TimedWait(timeout);
  }
  thread_.Stop();
}

}  // namespace avbase
