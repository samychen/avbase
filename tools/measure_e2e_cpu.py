#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# End-to-end CPU cost of a full play-through: the machine half of the
# docs/04 §8 gate that used to be "run tools/bench_render.sh and eyeball it".
# It answers one question -- how much CPU does one second of 1080p30 cost when
# the whole pipeline runs -- and writes it as a comparable number.
#
# WHAT IS MEASURED, AND WHY THAT UNIT
#   The child process's CPU time (user + sys, getrusage(RUSAGE_CHILDREN))
#   divided by the clip's media duration: cpu_ms_per_media_s.
#
#   * Not wall time: playback is real-time throttled (a 10 s clip takes ~10 s
#     wall whether the decoder got 20% faster or not), so wall is a constant
#     with noise on top.
#   * Not RSS: this sandbox forbids reading another process's RSS, and CPU --
#     not memory -- is the quantity docs/04 §8 budgets ("relative to raw
#     FFmpeg, total extra cost < 3% CPU").
#   * Divided by media seconds so that changing the clip length does not look
#     like a regression. The dividend is the clip's own duration, not the
#     wall time of the run.
#
# WHY THE CLIP IS GENERATED AND NOT COMMITTED
#   A 1080p30 H.264 clip is tens of megabytes; none belongs in git. ffmpeg
#   builds it once into $TMPDIR and every later run reuses it, and the file
#   name carries its parameters so changing them regenerates instead of
#   silently measuring a stale clip.
#
# MEASURED NOISE FLOOR (the reason the threshold is not 5%)
#   docs/04 §8 sets "end-to-end CPU regression > 5% fails". On this machine,
#   same binary, no code change: a single run of the 1080p30 10 s clip moved
#   between 45.7 and 53.2 ms per media second (8-16% spread), and the medians
#   of two consecutive groups of five runs differed by 8.1%. The noise is
#   *proportional*, not a fixed start-up cost -- it did not shrink when the
#   clip got 3x heavier, which rules out "average the start-up away". So 5% is
#   kept as the design target in docs/04 §8 and the machine threshold lives in
#   tools/e2e-cpu-tolerance.json at 15%, for the same reason the
#   micro-benchmarks judge at 25%: a gate that fires on noise trains the next
#   person to override it, and then it catches nothing.
#
# Usage:
#   tools/measure_e2e_cpu.py --jsonl /tmp/e2e-cpu.jsonl
#   tools/measure_e2e_cpu.py --headless build/ffmpeg/bin/headless --runs 5 \
#       --jsonl /tmp/e2e-cpu.jsonl
#   tools/measure_e2e_cpu.py --selftest

import argparse
import json
import os
import resource
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

# The clip: 1080p30 H.264, the resolution and rate docs/04 §8 writes its whole
# budget table against ("1080p30 + 48 kHz stereo, frame interval 33.3 ms").
DEFAULT_SIZE = "1920x1080"
DEFAULT_RATE = "30"
DEFAULT_BITRATE = "8M"
DEFAULT_SECONDS = 10


def cpu_seconds_of(argv, timeout):
    """Run `argv`, return (child CPU seconds, return code).

    RUSAGE_CHILDREN accumulates every child that has been waited for, so the
    value is a delta around exactly one run and this function must never be
    called concurrently.
    """
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    proc = subprocess.run(argv, capture_output=True, timeout=timeout)
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime)
    return cpu, proc.returncode


def ensure_clip(ffmpeg, clip_dir, seconds, size, rate, bitrate):
    """Build the clip once; reuse it afterwards. Returns its path."""
    clip = clip_dir / f"{size.replace('x', 'x')}-{rate}fps-{seconds}s.mp4"
    if clip.exists():
        return clip
    if not ffmpeg:
        raise SystemExit("ffmpeg is not on PATH; it is what builds the clip")
    clip_dir.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
         "-i", f"testsrc2=size={size}:rate={rate}", "-t", str(seconds),
         "-pix_fmt", "yuv420p", "-c:v", "libx264", "-preset", "veryfast",
         "-b:v", bitrate, str(clip)],
        check=True, timeout=600)
    return clip


def measure(headless, clip, media_seconds, runs, timeout):
    """Return the list of per-run cpu_ms_per_media_s, or raise on failure."""
    values = []
    for _ in range(runs):
        cpu, rc = cpu_seconds_of(
            [headless, str(clip), "--timeout", str(timeout)], timeout + 30)
        # A run that did not reach kCompleted is not a fast run, it is a broken
        # one: headless exits 0 only on kCompleted, and treating a failure as
        # an improvement is how a gate ends up certifying a player that does
        # not play.
        if rc != 0:
            raise SystemExit(
                f"headless exited {rc} on {clip}; refusing to record a number "
                "for a run that did not complete")
        values.append(cpu * 1000.0 / media_seconds)
    return values


def selftest() -> int:
    """Prove the pieces that can silently lie, without a real player.

    A fake headless (a shell that exits 0) exercises the happy path; one that
    exits 1 must be refused rather than recorded as a very cheap play-through.
    """
    with tempfile.TemporaryDirectory() as d:
        ok = Path(d) / "fake_ok.sh"
        bad = Path(d) / "fake_bad.sh"
        ok.write_text("#!/bin/sh\nexit 0\n")
        bad.write_text("#!/bin/sh\nexit 1\n")
        ok.chmod(0o755)
        bad.chmod(0o755)
        clip = Path(d) / "clip.mp4"
        clip.write_bytes(b"")

        values = measure(str(ok), clip, 10.0, 3, 5)
        if len(values) != 3 or any(v < 0 for v in values):
            print(f"selftest FAIL: unexpected values {values}")
            return 1
        try:
            measure(str(bad), clip, 10.0, 1, 5)
        except SystemExit as e:
            if "did not complete" not in str(e):
                print(f"selftest FAIL: wrong refusal {e}")
                return 1
        else:
            print("selftest FAIL: a failed play-through was recorded")
            return 1

    # The unit conversion: 0.5 CPU seconds over a 10 s clip is 50 ms per media
    # second. Off-by-1000 here would still produce a plausible-looking number.
    row = {"bench": "E2E_CPU_1080p30_10s",
           "cpu_ms_per_media_s": 0.5 * 1000.0 / 10.0}
    if abs(row["cpu_ms_per_media_s"] - 50.0) > 1e-9:
        print(f"selftest FAIL: unit conversion gave {row}")
        return 1

    # Median over runs is what the gate compares; make sure an outlier run
    # cannot decide the number (the whole reason for --runs > 1).
    if statistics.median([48.0, 49.0, 120.0]) != 49.0:
        print("selftest FAIL: median of runs is wrong")
        return 1
    print("selftest OK")
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--headless", default="build/ffmpeg/bin/headless",
                   help="headless player binary (exits 0 on kCompleted)")
    p.add_argument("--clip", default=None,
                   help="play this file instead of generating one")
    p.add_argument("--media-seconds", type=float, default=DEFAULT_SECONDS,
                   help="duration the cpu is divided by (default: 10)")
    p.add_argument("--runs", type=int, default=5,
                   help="play-throughs to take the median of (default: 5)")
    p.add_argument("--jsonl", type=Path, default=None,
                   help="append one JSON line per run (default: stdout)")
    p.add_argument("--label", default=None,
                   help="benchmark name (default: derived from the clip)")
    p.add_argument("--clip-dir", default=None,
                   help="where generated clips are cached "
                        "(default: $TMPDIR/avbase-e2e-cpu)")
    p.add_argument("--selftest", action="store_true",
                   help="run built-in checks; needs no player or ffmpeg")
    args = p.parse_args(argv)

    if args.selftest:
        return selftest()

    headless = Path(args.headless)
    if not headless.exists():
        print(f"no headless at {headless}; build it with the ffmpeg preset",
              file=sys.stderr)
        return 2

    if args.clip:
        clip = Path(args.clip)
        if not clip.exists():
            print(f"clip not found: {clip}", file=sys.stderr)
            return 2
        label = args.label or clip.stem
    else:
        clip_dir = Path(args.clip_dir) if args.clip_dir else Path(
            tempfile.gettempdir()) / "avbase-e2e-cpu"
        clip = ensure_clip(shutil.which("ffmpeg"), clip_dir,
                           int(args.media_seconds), DEFAULT_SIZE, DEFAULT_RATE,
                           DEFAULT_BITRATE)
        label = args.label or f"E2E_CPU_{DEFAULT_SIZE}_{DEFAULT_RATE}fps_" \
                              f"{int(args.media_seconds)}s"

    # Generous but finite: a clip that takes three times its own length has
    # stopped being a measurement and become a hang.
    timeout = int(args.media_seconds) * 3 + 30
    values = measure(str(headless), clip, args.media_seconds, args.runs,
                     timeout)

    out = args.jsonl.open("a", encoding="utf-8") if args.jsonl else None
    try:
        for v in values:
            row = {"bench": label, "cpu_ms_per_media_s": round(v, 3),
                   "media_seconds": args.media_seconds, "runs": args.runs,
                   "clip": clip.name}
            line = json.dumps(row, sort_keys=True)
            if out:
                out.write(line + "\n")
            else:
                print(line)
    finally:
        if out:
            out.close()

    print(f"{label}: median {statistics.median(values):.1f} ms per media "
          f"second over {args.runs} run(s) "
          f"(min {min(values):.1f}, max {max(values):.1f})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
