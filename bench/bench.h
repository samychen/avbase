// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Self-contained micro-benchmark harness. Deliberately has NO third-party
// dependency (Google Benchmark cannot be fetched in the sandbox, and the CI
// runner should not have to install it). It mirrors the subset of the
// docs/04 §8 `BM_*` list that is cheap and stable enough to track.
//
// Each benchmark body receives an iteration count and must perform exactly one
// logical "operation" per iteration so the per-op cost is meaningful.

#ifndef AVBASE_BENCH_BENCH_H_
#define AVBASE_BENCH_BENCH_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace avbase::bench {

// A benchmark case: invoked with a requested iteration count.
using BenchFn = std::function<void(uint64_t iters)>;

struct Case {
  std::string name;
  BenchFn fn;
};

// Static registry populated by the AVBASE_BENCH macro at static-init time.
std::vector<Case>& Registry();

// Result of timing a benchmark.
struct Stats {
  double mean_ns_per_op = 0.0;  // mean over all timed rounds
  double p75_ns_per_op = 0.0;   // 75th percentile of per-round ns/op
};

// Times `fn` for `iters` ops, repeated `rounds` times. Per-round ns/op is
// collected (not per-op) so memory stays bounded even at 1e6 iterations; the
// p75 of rounds smooths scheduler noise far cheaper than storing every op.
Stats Measure(const BenchFn& fn, uint64_t iters, int rounds = 21);

// Registers a benchmark. Place in an anonymous namespace; the macro emits the
// static function declaration, a static Registrar, and the definition.
struct Registrar {
  Registrar(const char* name, BenchFn fn);
};

}  // namespace avbase::bench

#define AVBASE_BENCH(name)                                         \
  static void name(uint64_t iters);                                \
  static ::avbase::bench::Registrar name##_registrar(#name, name); \
  static void name(uint64_t iters)

#endif  // AVBASE_BENCH_BENCH_H_
