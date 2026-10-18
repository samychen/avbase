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
  double mean_ns_per_op = 0.0;   // mean over all timed rounds
  double p75_ns_per_op = 0.0;    // 75th percentile of per-round ns/op
  uint64_t iters_per_round = 0;  // what was actually timed, after scaling
};

// Times `fn` for `iters` ops, repeated `rounds` times. Per-round ns/op is
// collected (not per-op) so memory stays bounded even at 1e6 iterations; the
// p75 of rounds smooths scheduler noise far cheaper than storing every op.
//
// `iters` is a FLOOR, not the count used. One probe round estimates the per-op
// cost and the count is then scaled up so each round lasts at least
// `min_round_ms`. Without this the fast benchmarks are unmeasurable: a 5 ns op
// at 50 000 iterations is a 250 microsecond round, and a single scheduler tick
// inside it moves the answer by tens of percent -- measured, three consecutive
// runs of BM_GetMasterClock on an idle machine reported 5.3 ns, 13.6 ns and
// 6.4 ns for identical code. Scaling is what makes the 10% regression
// threshold in tools/check_bench_regression.py mean anything.
Stats Measure(const BenchFn& fn, uint64_t iters, int rounds = 21,
              double min_round_ms = 20.0);

// Writes a value somewhere the optimiser cannot see through.
//
// Several benchmark bodies are pure functions of constant input, so a compiler
// is entitled to delete a loop whose result is unused -- which would report
// 0 ns/op, a green result produced by measuring nothing.
//
// Honest status: this is a guard against a future compiler, not a load-bearing
// part of today's numbers. The tree does not enable LTO (no
// INTERPROCEDURAL_OPTIMIZATION anywhere, and no -flto in the compile line --
// docs/08 lists -flto only as a *candidate* R3 mitigation, never adopted), and
// the timed functions live in other translation units. Mutation-checked: with
// every Sink() call replaced by `(void)sink`, BM_DecideNextFrame measured
// 17.4 ns against 16.2 ns with them, and BM_GetMasterClock 6.7 ns against
// 6.8 ns -- inside the noise, i.e. nothing was being deleted. Keep it so that
// turning on LTO cannot silently turn these into measurements of nothing.
inline void Sink(uint64_t value) {
  static volatile uint64_t slot = 0;
  slot = value;
}

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
