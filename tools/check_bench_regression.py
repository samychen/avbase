#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Micro-benchmark trend guard (docs/07 §10, docs/08 R3). Compares the current
# benchmark run against a git-tracked baseline and flags regressions.
#
# Design notes (kept honest on purpose):
#   * This is a TREND GUARD, not a hard per-PR gate. CI machines are shared and
#     noisy, so a >10% wobble is common; we therefore default to reporting and
#     only fail when --fail-on-regression is passed (developer-local use, where
#     the machine is quieter and the result is actionable).
#   * The baseline is a median of the recorded rows per benchmark, robust to a
#     single outlier run. The "current" side is a single run by default.
#   * --update-baseline rewrites the baseline file with the current rows (one
#     canonical row per benchmark). Only do this on main after an intentional
#     change, never from CI.
#
# Usage:
#   tools/check_bench_regression.py --baseline tools/bench-baseline.jsonl \
#       --current /tmp/bench.jsonl
#   tools/check_bench_regression.py --baseline tools/bench-baseline.jsonl \
#       --current /tmp/bench.jsonl --fail-on-regression
#   tools/check_bench_regression.py --baseline tools/bench-baseline.jsonl \
#       --current /tmp/bench.jsonl --update-baseline
#   tools/check_bench_regression.py --selftest

import argparse
import json
import statistics
import sys
import tempfile
from pathlib import Path


def load_rows(path: Path) -> list[dict]:
    rows = []
    text = path.read_text(encoding="utf-8")
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        rows.append(json.loads(line))
    return rows


def baseline_median(rows: list[dict]) -> dict[str, float]:
    by_bench: dict[str, list[float]] = {}
    for r in rows:
        by_bench.setdefault(r["bench"], []).append(float(r["mean_ns_per_op"]))
    return {b: statistics.median(v) for b, v in by_bench.items()}


def current_mean(rows: list[dict]) -> dict[str, float]:
    out = {}
    for r in rows:
        out[r["bench"]] = float(r["mean_ns_per_op"])
    return out


def check(baseline_path: Path, current_path: Path, threshold: float,
          fail_on_regression: bool) -> int:
    base_rows = load_rows(baseline_path) if baseline_path.exists() else []
    cur_rows = load_rows(current_path)
    if not cur_rows:
        print("check_bench_regression: no current rows; nothing to do")
        return 0

    base = baseline_median(base_rows)
    cur = current_mean(cur_rows)

    print(f"{'benchmark':<44} {'baseline':>12} {'current':>12} {'delta':>9}  flag")
    print("-" * 86)
    regressions = 0
    for bench in sorted(cur):
        c = cur[bench]
        if bench not in base:
            print(f"{bench:<44} {'—':>12} {c:>12.1f} {'NEW':>9}")
            continue
        b = base[bench]
        delta = (c - b) / b if b else 0.0
        flag = ""
        if delta > threshold:
            flag = "REGRESSION"
            regressions += 1
        elif delta < -threshold:
            flag = "improvement"
        else:
            flag = "ok"
        print(f"{bench:<44} {b:>12.1f} {c:>12.1f} {delta:>+8.1%}  {flag}")

    print(f"\nbenchmarks: {len(cur)}  regressions(>{threshold:.0%}): {regressions}")
    if regressions and fail_on_regression:
        print("FAIL: performance regression(s) exceed threshold")
        return 1
    if regressions:
        print("NOTE: regression(s) reported but not failing (trend guard is "
              "soft by default; pass --fail-on-regression locally to gate)")
    return 0


def update_baseline(baseline_path: Path, current_path: Path) -> None:
    rows = load_rows(current_path)
    baseline_path.parent.mkdir(parents=True, exist_ok=True)
    with baseline_path.open("w", encoding="utf-8") as f:
        for r in rows:
            f.write(json.dumps(r, sort_keys=True) + "\n")
    print(f"baseline updated: {baseline_path} ({len(rows)} benchmarks)")


def selftest() -> int:
    base = [
        {"bench": "BM_A", "iterations": 1000, "rounds": 5,
         "mean_ns_per_op": 100.0, "p75_ns_per_op": 105.0},
        {"bench": "BM_B", "iterations": 1000, "rounds": 5,
         "mean_ns_per_op": 200.0, "p75_ns_per_op": 210.0},
    ]
    # Current: BM_A regresses 20% (>10%), BM_B improves.
    cur = [
        {"bench": "BM_A", "iterations": 1000, "rounds": 5,
         "mean_ns_per_op": 120.0, "p75_ns_per_op": 125.0},
        {"bench": "BM_B", "iterations": 1000, "rounds": 5,
         "mean_ns_per_op": 180.0, "p75_ns_per_op": 185.0},
    ]
    with tempfile.TemporaryDirectory() as d:
        bp = Path(d) / "base.jsonl"
        cp = Path(d) / "cur.jsonl"
        bp.write_text("\n".join(json.dumps(r) for r in base) + "\n")
        cp.write_text("\n".join(json.dumps(r) for r in cur) + "\n")
        rc = check(bp, cp, 0.10, fail_on_regression=True)
        if rc != 1:
            print(f"selftest FAIL: expected rc=1 with --fail-on-regression, got {rc}")
            return 1
        rc2 = check(bp, cp, 0.10, fail_on_regression=False)
        if rc2 != 0:
            print(f"selftest FAIL: expected rc=0 without --fail-on-regression, got {rc2}")
            return 1
    print("selftest OK")
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--baseline", type=Path, help="git-tracked baseline JSONL")
    p.add_argument("--current", type=Path, help="current run JSONL")
    p.add_argument("--threshold", type=float, default=0.10,
                   help="regression threshold (default 0.10 = 10%%)")
    p.add_argument("--fail-on-regression", action="store_true",
                   help="exit 1 if any benchmark regresses beyond threshold")
    p.add_argument("--update-baseline", action="store_true",
                   help="rewrite the baseline file with current rows")
    p.add_argument("--selftest", action="store_true", help="run built-in checks")
    args = p.parse_args(argv)

    if args.selftest:
        return selftest()

    if not args.baseline or not args.current:
        print("error: --baseline and --current are required (or --selftest)",
              file=sys.stderr)
        return 2

    if args.update_baseline:
        update_baseline(args.baseline, args.current)
        return 0

    return check(args.baseline, args.current, args.threshold,
                args.fail_on_regression)


if __name__ == "__main__":
    raise SystemExit(main())
