// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task/sequenced_task_runner.h"

#include <utility>

namespace avbase::base {
namespace {

// One default runner per thread. The slot is a plain thread_local rather than a
// heap-allocated leak (Chromium's choice) so that LeakSanitizer stays clean;
// avbase never touches the current default runner from an exit handler, and
// base::Thread clears it before its sequence ends.
scoped_refptr<SequencedTaskRunner>& CurrentDefaultSlot() {
  static thread_local scoped_refptr<SequencedTaskRunner> slot;
  return slot;
}

}  // namespace

SequencedTaskRunner::SequencedTaskRunner() = default;
SequencedTaskRunner::~SequencedTaskRunner() = default;

// static
scoped_refptr<SequencedTaskRunner> SequencedTaskRunner::GetCurrentDefault() {
  return CurrentDefaultSlot();
}

// static
void SequencedTaskRunner::SetCurrentDefault(
    scoped_refptr<SequencedTaskRunner> runner) {
  CurrentDefaultSlot() = std::move(runner);
}

// static
bool SequencedTaskRunner::HasCurrentDefault() {
  return static_cast<bool>(CurrentDefaultSlot());
}

}  // namespace avbase::base
