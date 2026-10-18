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

Stats Measure(const BenchFn& fn, uint64_t iters, int rounds) {
  // Warm up to settle caches and branch predictors before timing.
  fn(64);

  std::vector<double> per_round_ns;
  per_round_ns.reserve(rounds);
  double sum = 0.0;
  for (int r = 0; r < rounds; ++r) {
    auto t0 = std::chrono::steady_clock::now();
    fn(iters);
    auto t1 = std::chrono::steady_clock::now();
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    per_round_ns.push_back(ns / static_cast<double>(iters));
    sum += per_round_ns.back();
  }
  std::sort(per_round_ns.begin(), per_round_ns.end());
  Stats s;
  s.mean_ns_per_op = sum / static_cast<double>(rounds);
  size_t idx =
      static_cast<size_t>(0.75 * static_cast<double>(rounds - 1) + 0.5);
  s.p75_ns_per_op = per_round_ns[idx];
  return s;
}

}  // namespace avbase::bench

int main(int argc, char** argv) {
  uint64_t iters = 200000;
  int rounds = 21;
  const char* jsonl_path = nullptr;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--iters" && i + 1 < argc) {
      iters = strtoull(argv[++i], nullptr, 10);
    } else if (a == "--rounds" && i + 1 < argc) {
      rounds = std::atoi(argv[++i]);
    } else if (a == "--jsonl" && i + 1 < argc) {
      jsonl_path = argv[++i];
    }
  }

  const auto& cases = avbase::bench::Registry();
  std::vector<avbase::bench::Stats> stats;
  stats.reserve(cases.size());

  printf("%-44s %14s %14s\n", "benchmark", "mean ns/op", "p75 ns/op");
  for (int i = 0; i < 74; ++i)
    putchar('-');
  putchar('\n');
  for (const auto& c : cases) {
    auto s = avbase::bench::Measure(c.fn, iters, rounds);
    stats.push_back(s);
    printf("%-44s %14.1f %14.1f\n", c.name.c_str(), s.mean_ns_per_op,
           s.p75_ns_per_op);
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
      fprintf(f,
              "{\"bench\":\"%s\",\"iterations\":%llu,\"rounds\":%d,"
              "\"mean_ns_per_op\":%.3f,\"p75_ns_per_op\":%.3f}\n",
              c.name.c_str(), (unsigned long long)iters, rounds,
              s.mean_ns_per_op, s.p75_ns_per_op);
    }
    std::fclose(f);
  }
  return 0;
}
