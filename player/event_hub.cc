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
  auto entry = std::make_shared<ObserverEntry>();
  entry->id = next_observer_id_++;
  entry->observer = observer;
  const int id = entry->id;
  observers_.push_back(std::move(entry));
  return id;
}

void EventHub::RemoveObserver(int id) {
  bool wait_for_dispatch = false;
  {
    base::AutoLock scoped(lock_);
    for (size_t i = 0; i < observers_.size(); ++i) {
      if (observers_[i]->id == id) {
        // Marked dead before being erased: a Dispatch that already snapshotted
        // this entry is about to check exactly this flag.
        observers_[i]->alive.store(false, std::memory_order_relaxed);
        observers_.erase(observers_.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
    // On the dispatch thread the in-flight dispatch is the caller's own
    // callback, so waiting would deadlock; the dead flag is enough there.
    wait_for_dispatch = dispatching_ > 0 && !OnDispatchThread();
  }
  // Off the dispatch thread, the observer may be destroyed the moment this
  // returns, so the dispatch that still names it must be allowed to finish
  // first. Re-checked after each wake: Dispatch re-arms drain_idle_ under the
  // lock, so a wake landing between Signal and the next Reset cannot lose the
  // update.
  while (wait_for_dispatch) {
    drain_idle_.TimedWait(base::Seconds(5));
    base::AutoLock scoped(lock_);
    wait_for_dispatch = dispatching_ > 0;
  }
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
  std::vector<std::shared_ptr<ObserverEntry>> observers;
  {
    base::AutoLock scoped(lock_);
    ++dispatching_;
    // Re-armed before the lock is dropped, so a RemoveObserver that is about
    // to wait cannot observe a stale signal from a previous dispatch.
    drain_idle_.Reset();
    handler = handler_;
    observers = observers_;
  }
  // The handler runs with no lock held: it may call any Player method, which
  // may itself post more events or try to take this same lock.
  if (handler) {
    handler.Run(event);
  }
  for (const std::shared_ptr<ObserverEntry>& entry : observers) {
    // The entry -- and the observer behind it -- may have been removed by the
    // handler above, by an earlier observer in this same loop, or by another
    // thread. The shared_ptr keeps the ENTRY readable; the flag says whether
    // the observer it names is still alive.
    if (entry->alive.load(std::memory_order_relaxed)) {
      entry->observer->OnEvent(event);
    }
  }
  {
    base::AutoLock scoped(lock_);
    if (--dispatching_ == 0) {
      drain_idle_.Signal();
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
