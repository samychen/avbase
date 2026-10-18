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
#   * Compared quantity is the p75 of rounds, not the mean -- see value_of().
#   * The baseline is a median of the recorded rows per benchmark, robust to a
#     single outlier run. Building it from several runs (append them all before
#     --update-baseline) is the intended workflow; one run decides too much.
#   * Per-benchmark noise floors live in tools/bench-tolerance.json. A 10%
#     threshold cannot serve both a 6 ns seqlock read and a cross-thread round
#     trip; see that file for the measurements behind the overrides.
#   * --update-baseline rewrites the baseline file with the current rows. Only
#     do this on main after an intentional change, never from CI.
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


def value_of(row: dict) -> float:
    """The number this gate compares: p75 of rounds, falling back to mean.

    Not the mean, deliberately. A round interrupted by a scheduler tick or by
    a background build drags the mean up while three quarters of the rounds
    sit exactly where they were -- observed directly: BM_BindOnce_Invoke
    measured 113/114/117 ns across three quiet runs and then 220 ns in one run
    taken while a compiler was still draining, whose own p75 was 163 ns. The
    mean remembered the interruption; the p75 mostly did not. Both sides are
    compared on the same statistic, so the choice costs no sensitivity.
    """
    v = row.get("p75_ns_per_op")
    if v is not None:
        return float(v)
    return float(row["mean_ns_per_op"])


def baseline_median(rows: list[dict]) -> dict[str, float]:
    """Median per benchmark, so a baseline built from several runs is not
    decided by whichever run happened to be unlucky."""
    by_bench: dict[str, list[float]] = {}
    for r in rows:
        by_bench.setdefault(r["bench"], []).append(value_of(r))
    return {b: statistics.median(v) for b, v in by_bench.items()}


def current_mean(rows: list[dict]) -> dict[str, float]:
    """Median per benchmark over the rows of the current run.

    Median, not "the row", because the input is allowed to be several
    consecutive runs appended together -- which is how the gate is meant to be
    driven (see job_bench in tools/gate_local.sh). Run-to-run spread is the
    dominant noise term here: widening the tolerance to absorb it would bury
    real regressions, whereas spending three runs costs seconds and removes
    it. One row behaves exactly as before.
    """
    by_bench: dict[str, list[float]] = {}
    for r in rows:
        by_bench.setdefault(r["bench"], []).append(value_of(r))
    return {b: statistics.median(v) for b, v in by_bench.items()}


def load_tolerances(path: Path) -> tuple[float, dict]:
    """Return (default_threshold, per_bench_overrides).

    A file that is missing or unreadable is not an error: the caller falls
    back to the global --threshold. This gate must never be able to fail
    because of its own configuration.
    """
    if not path.exists():
        return 0.10, {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return 0.10, {}
    default = float(data.get("default", 0.10))
    # Keys starting with "_" are prose, not benchmarks -- the tolerance file
    # carries the measurement behind each number, and a documentation key that
    # crashed the gate would be a gate that fails for a comment.
    per_bench = {}
    for k, v in data.get("per_bench", {}).items():
        if k.startswith("_"):
            continue
        try:
            per_bench[k] = float(v)
        except (TypeError, ValueError):
            continue
    return default, per_bench


def check(baseline_path: Path, current_path: Path, threshold: float,
          fail_on_regression: bool, per_bench: dict = None) -> int:
    base_rows = load_rows(baseline_path) if baseline_path.exists() else []
    cur_rows = load_rows(current_path)
    if not cur_rows:
        print("check_bench_regression: no current rows; nothing to do")
        return 0

    base = baseline_median(base_rows)
    cur = current_mean(cur_rows)

    per_bench = per_bench or {}
    print(f"{'benchmark':<40} {'baseline':>12} {'current':>12} {'delta':>9} "
          f"{'tol':>6}  flag")
    print("-" * 92)
    regressions = 0
    for bench in sorted(cur):
        c = cur[bench]
        if bench not in base:
            print(f"{bench:<40} {'—':>12} {c:>12.1f} {'NEW':>9}")
            continue
        b = base[bench]
        delta = (c - b) / b if b else 0.0
        # Per-benchmark noise floor where one is configured; see
        # tools/bench-tolerance.json for why a single number cannot serve both
        # a 6 ns seqlock read and a cross-thread round trip.
        tol = per_bench.get(bench, threshold)
        flag = ""
        if delta > tol:
            flag = "REGRESSION"
            regressions += 1
        elif delta < -tol:
            flag = "improvement"
        else:
            flag = "ok"
        print(f"{bench:<40} {b:>12.1f} {c:>12.1f} {delta:>+8.1%} "
              f"{tol:>6.0%}  {flag}")

    print(f"\nbenchmarks: {len(cur)}  regressions: {regressions}")
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

    # Per-benchmark tolerance, checked in both directions. BM_C regresses 20%
    # -- over the global 10%, under its own 40% noise floor. With the override
    # it must pass; without it, it must fail. A tolerance that only ever
    # passes is indistinguishable from a threshold someone quietly raised, so
    # the "still fails without the override" half is the one that carries the
    # evidence.
    base_c = [{"bench": "BM_C", "iterations": 1000, "rounds": 5,
               "mean_ns_per_op": 10000.0, "p75_ns_per_op": 10100.0}]
    cur_c = [{"bench": "BM_C", "iterations": 1000, "rounds": 5,
              "mean_ns_per_op": 12000.0, "p75_ns_per_op": 12100.0}]
    with tempfile.TemporaryDirectory() as d:
        bp = Path(d) / "base.jsonl"
        cp = Path(d) / "cur.jsonl"
        bp.write_text("\n".join(json.dumps(r) for r in base_c) + "\n")
        cp.write_text("\n".join(json.dumps(r) for r in cur_c) + "\n")
        relaxed = check(bp, cp, 0.10, True, per_bench={"BM_C": 0.40})
        if relaxed != 0:
            print(f"selftest FAIL: 20% under a 40% tolerance should pass, "
                  f"got {relaxed}")
            return 1
        strict = check(bp, cp, 0.10, True, per_bench={})
        if strict != 1:
            print(f"selftest FAIL: 20% with no override should fail, "
                  f"got {strict}")
            return 1
    # Tolerance parsing: "_"-prefixed keys are prose and must never reach
    # float(). Not a hypothetical -- the per_bench explanation for
    # BM_PostTask_RoundTrip is a sentence, and it took the whole gate down with
    # a ValueError on the first run that introduced it.
    with tempfile.TemporaryDirectory() as d:
        tp = Path(d) / "tol.json"
        tp.write_text(json.dumps({
            "_comment": "prose",
            "default": 0.25,
            "per_bench": {"_BM_X": "a sentence, not a number", "BM_X": 0.40},
        }))
        dflt, per = load_tolerances(tp)
        if dflt != 0.25 or per != {"BM_X": 0.40}:
            print(f"selftest FAIL: tolerance parse gave {dflt!r}, {per!r}")
            return 1
        dflt2, per2 = load_tolerances(Path(d) / "absent.json")
        if dflt2 != 0.10 or per2 != {}:
            print(f"selftest FAIL: absent tolerance file should fall back to "
                  f"(0.10, {{}}), got {dflt2!r}, {per2!r}")
            return 1

    print("selftest OK")
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--baseline", type=Path, help="git-tracked baseline JSONL")
    p.add_argument("--current", type=Path, help="current run JSONL")
    p.add_argument("--threshold", type=float, default=None,
                   help="global regression threshold (default: 'default' "
                        "from the tolerance file, else 0.10 = 10%%)")
    p.add_argument("--tolerance", type=Path,
                   default=Path(__file__).resolve().parent /
                   "bench-tolerance.json",
                   help="JSON file with per-benchmark noise floors")
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

    default_tol, per_bench = load_tolerances(args.tolerance)
    threshold = args.threshold if args.threshold is not None else default_tol
    return check(args.baseline, args.current, threshold,
                 args.fail_on_regression, per_bench)


if __name__ == "__main__":
    raise SystemExit(main())
