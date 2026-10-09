// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/event_hub.h"

#include <cstddef>
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
  const int id = static_cast<int>(observers_.size()) + 1;
  observers_.push_back(ObserverEntry{id, observer});
  return id;
}

void EventHub::RemoveObserver(int id) {
  base::AutoLock scoped(lock_);
  for (size_t i = 0; i < observers_.size(); ++i) {
    if (observers_[i].id == id) {
      observers_.erase(observers_.begin() + static_cast<std::ptrdiff_t>(i));
      return;
    }
  }
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
  std::vector<PlayerObserver*> observers;
  {
    base::AutoLock scoped(lock_);
    handler = handler_;
    for (const ObserverEntry& entry : observers_) {
      observers.push_back(entry.observer);
    }
  }
  // The handler runs with no lock held: it may call any Player method, which
  // may itself post more events or try to take this same lock.
  if (handler) {
    handler.Run(event);
  }
  for (PlayerObserver* observer : observers) {
    observer->OnEvent(event);
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
