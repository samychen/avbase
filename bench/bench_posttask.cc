// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// BM_PostTask_RoundTrip: cost of posting a task to another thread and waiting
// for it to run (docs/04 §8 budget: PostTask < 2 us). Mirrors the round-trip
// shape every cross-thread hand-off in the player pays.

#include "bench.h"

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"

namespace {

AVBASE_BENCH(BM_PostTask_RoundTrip) {
  avbase::base::Thread thread("bench-posttask");
  thread.Start();
  auto runner = thread.task_runner();
  avbase::base::WaitableEvent done(
      avbase::base::WaitableEvent::ResetPolicy::kManualReset,
      avbase::base::WaitableEvent::InitialState::kNotSignaled);
  for (uint64_t i = 0; i < iters; ++i) {
    done.Reset();
    runner->PostTask(
        FROM_HERE,
        avbase::base::BindOnce(
            [](avbase::base::WaitableEvent* e) { e->Signal(); }, &done));
    done.Wait();
  }
  thread.Stop();
}

}  // namespace
