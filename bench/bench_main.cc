// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Entry point for the self-contained micro-benchmark harness. Runs every
// registered benchmark, prints a table, and optionally appends one JSONL row
// per benchmark for trend tracking (docs/07 §10). No external dependency.

#include "bench.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace avbase::bench {

std::vector<Case>& Registry() {
  static std::vector<Case> r;
  return r;
}

Registrar::Registrar(const char* name, BenchFn fn) {
  Registry().push_back({name, std::move(fn)});
}

namespace {

using Clock = std::chrono::steady_clock;

// Upper bound on a single round's iteration count. Caps the cost of a
// benchmark whose per-op time collapses (a fast path that got faster): 2e8
// ops of anything measurable is already seconds per round.
constexpr uint64_t kMaxIters = 200000000ull;

uint64_t ScaleIters(double probe_ns_per_op, uint64_t floor_iters,
                    double min_round_ms) {
  if (!(probe_ns_per_op > 0.0))
    return floor_iters;  // Probe produced nothing usable; keep the request.
  const double want = (min_round_ms * 1e6) / probe_ns_per_op;
  if (want <= static_cast<double>(floor_iters))
    return floor_iters;  // Already long enough; never scale DOWN.
  if (want >= static_cast<double>(kMaxIters))
    return kMaxIters;
  return static_cast<uint64_t>(want);
}

}  // namespace

Stats Measure(const BenchFn& fn, uint64_t iters, int rounds,
              double min_round_ms) {
  // Warm up to settle caches and branch predictors before timing.
  fn(64);

  // Probe round: discarded, used only to size the real ones. See bench.h for
  // why a fixed iteration count is not enough.
  const auto p0 = Clock::now();
  fn(iters);
  const auto p1 = Clock::now();
  const double probe_ns =
      std::chrono::duration<double, std::nano>(p1 - p0).count() /
      static_cast<double>(iters);
  const uint64_t n = ScaleIters(probe_ns, iters, min_round_ms);

  std::vector<double> per_round_ns;
  per_round_ns.reserve(rounds);
  double sum = 0.0;
  for (int r = 0; r < rounds; ++r) {
    auto t0 = Clock::now();
    fn(n);
    auto t1 = Clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    per_round_ns.push_back(ns / static_cast<double>(n));
    sum += per_round_ns.back();
  }
  std::sort(per_round_ns.begin(), per_round_ns.end());
  Stats s;
  s.mean_ns_per_op = sum / static_cast<double>(rounds);
  size_t idx =
      static_cast<size_t>(0.75 * static_cast<double>(rounds - 1) + 0.5);
  s.p75_ns_per_op = per_round_ns[idx];
  s.iters_per_round = n;
  return s;
}

}  // namespace avbase::bench

int main(int argc, char** argv) {
  uint64_t iters = 200000;
  int rounds = 21;
  double min_round_ms = 20.0;
  const char* jsonl_path = nullptr;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--iters" && i + 1 < argc) {
      iters = strtoull(argv[++i], nullptr, 10);
    } else if (a == "--rounds" && i + 1 < argc) {
      rounds = std::atoi(argv[++i]);
    } else if (a == "--min-round-ms" && i + 1 < argc) {
      min_round_ms = std::atof(argv[++i]);
    } else if (a == "--jsonl" && i + 1 < argc) {
      jsonl_path = argv[++i];
    }
  }

  const auto& cases = avbase::bench::Registry();
  std::vector<avbase::bench::Stats> stats;
  stats.reserve(cases.size());

  printf("%-40s %12s %12s %14s\n", "benchmark", "mean ns/op", "p75 ns/op",
         "iters/round");
  for (int i = 0; i < 82; ++i)
    putchar('-');
  putchar('\n');
  for (const auto& c : cases) {
    auto s = avbase::bench::Measure(c.fn, iters, rounds, min_round_ms);
    stats.push_back(s);
    printf("%-40s %12.1f %12.1f %14llu\n", c.name.c_str(), s.mean_ns_per_op,
           s.p75_ns_per_op, (unsigned long long)s.iters_per_round);
  }

  if (jsonl_path) {
    FILE* f = std::fopen(jsonl_path, "a");
    if (!f) {
      fprintf(stderr, "bench: cannot open %s for append\n", jsonl_path);
      return 1;
    }
    // Minimal hand-rolled JSON (no external lib); all fields are scalars.
    for (size_t i = 0; i < cases.size(); ++i) {
      const auto& c = cases[i];
      const auto& s = stats[i];
      // `iterations` is what was actually timed, not the requested floor: the
      // trend file has to be readable as "this number came from this much
      // work", or a later reader cannot tell a real regression from a change
      // in how long each round ran.
      fprintf(f,
              "{\"bench\":\"%s\",\"iterations\":%llu,\"rounds\":%d,"
              "\"mean_ns_per_op\":%.3f,\"p75_ns_per_op\":%.3f}\n",
              c.name.c_str(), (unsigned long long)s.iters_per_round, rounds,
              s.mean_ns_per_op, s.p75_ns_per_op);
    }
    std::fclose(f);
  }
  return 0;
}
