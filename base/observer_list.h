// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/observer_list.h` (BSD-3-Clause).

#ifndef AVBASE_BASE_OBSERVER_LIST_H_
#define AVBASE_BASE_OBSERVER_LIST_H_

#include <algorithm>
#include <cstddef>
#include <vector>

#include "base/check.h"

namespace avbase::base {

// A list of non-owning observer pointers that tolerates modification during
// iteration, matching Chromium's default ObserverListPolicy::kAll:
//   * an observer removed while the list is being iterated is NOT notified
//     again in the current pass;
//   * an observer added during iteration IS notified in the current pass.
// Removal is deferred until the outermost Iterator is destroyed, so the vector
// is never mutated underneath an active index.
//
// Not thread-safe by design; guard it with a Lock at the call site or confine
// it to one sequence (avbase does the latter for player::EventHub).
template <typename ObserverType>
class ObserverList {
 public:
  ObserverList() = default;
  ObserverList(const ObserverList&) = delete;
  ObserverList& operator=(const ObserverList&) = delete;
  ~ObserverList() = default;

  void AddObserver(ObserverType* observer) {
    CHECK(observer);
    CHECK(!HasObserver(observer)) << "observer added twice";
    list_.emplace_back(observer, /*alive=*/true);
  }

  void RemoveObserver(ObserverType* observer) {
    for (auto& entry : list_) {
      if (entry.observer == observer) {
        if (iteration_depth_ > 0) {
          entry.alive =
              false;  // Deferred erase; compacted when iteration ends.
        } else {
          list_.erase(
              std::remove_if(list_.begin(), list_.end(),
                             [&](const Entry& e) { return &e == &entry; }),
              list_.end());
        }
        return;
      }
    }
  }

  bool HasObserver(const ObserverType* observer) const {
    return std::any_of(list_.begin(), list_.end(),
                       [&](const Entry& e) { return e.observer == observer; });
  }

  bool empty() const { return size() == 0; }
  size_t size() const {
    // std::count_if returns difference_type (signed) because it shares its
    // return type with the distance algorithms; a count is never negative, so
    // the cast cannot truncate. It is explicit because -Wsign-conversion (debug
    // preset, -Werror) rejects the implicit narrowing.
    const auto alive = std::count_if(list_.begin(), list_.end(),
                                     [](const Entry& e) { return e.alive; });
    return static_cast<size_t>(alive);
  }
  void Clear() { list_.clear(); }

  class Iterator {
   public:
    explicit Iterator(ObserverList& list) : list_(&list) {
      ++list_->iteration_depth_;
      index_ = 0;
    }
    Iterator(const Iterator&) = delete;
    Iterator& operator=(const Iterator&) = delete;
    ~Iterator() {
      if (--list_->iteration_depth_ == 0) {
        list_->Compact();
      }
    }

    ObserverType* GetNext() {
      while (index_ < list_->list_.size()) {
        Entry& entry = list_->list_[index_++];
        if (entry.alive) {
          return entry.observer;
        }
      }
      return nullptr;
    }

   private:
    ObserverList* list_;
    size_t index_;
  };

 private:
  struct Entry {
    ObserverType* observer;
    bool alive;
  };

  void Compact() {
    list_.erase(std::remove_if(list_.begin(), list_.end(),
                               [](const Entry& e) { return !e.alive; }),
                list_.end());
  }

  std::vector<Entry> list_;
  int iteration_depth_ = 0;
};

}  // namespace avbase::base

#endif  // AVBASE_BASE_OBSERVER_LIST_H_
