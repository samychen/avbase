// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// BM_BindOnce_Invoke: cost of binding a one-arg lambda and invoking it once.
// Pure synchronous path (no thread); it is the closure-move/invoke half of the
// docs/04 §8 PostTask < 2 us budget.

#include "bench.h"

#include "base/functional/bind.h"
#include "base/functional/callback.h"

namespace {

AVBASE_BENCH(BM_BindOnce_Invoke) {
  int sink = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    avbase::base::OnceClosure cb =
        avbase::base::BindOnce([](int* s, int v) { *s += v; }, &sink, 1);
    std::move(cb).Run();
  }
  (void)sink;
}

}  // namespace
