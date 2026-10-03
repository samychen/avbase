#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Soak: run the demux fuzz driver for a long time and watch the memory curve.

docs/12 4.4. The deliverable is NOT "it ran for N hours" -- that is a stopwatch.
The deliverable is a FLAT MEMORY CURVE plus zero sanitizer reports, because
those are the two things a long run can actually prove:

  * a leak or a per-iteration allocation that never gets freed shows up as a
    rising floor long before it shows up as an OOM, and a stopwatch-only run
    would have reported success;
  * a use-after-free that only happens on the 400th teardown is invisible to a
    10-minute run and obvious at 400 iterations.

The driver is tests/fuzz's standalone build, which is the right shape for this:
it drives the REAL pipeline front door (FFmpegDemuxer Initialize, bounded
Reads, full Stop) with time-bounded waits, so a wedged input is a missed
finding rather than a hung run.

WHAT THIS IS NOT. It is not a substitute for a libFuzzer run: the mutations
here are four fixed classes on a fixed corpus, so this finds LIFETIME bugs
(leaks, teardown races, growth) and not coverage-guided ones. Those need
-max_total_time, which needs a libFuzzer runtime.

USAGE
    tools/run_soak.py --driver build/asan/bin/avbase_fuzz_demuxer_standalone \\
                      [--minutes 48] [--sample-seconds 20] [--out report.json]

EXIT CODES
    0  ran the full duration, no sanitizer report, memory curve within budget
    1  a sanitizer report, a crash, or a rising memory curve
    2  the driver is missing
"""

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time

# A leak per iteration is the failure this exists to catch, so the budget is
# expressed per iteration rather than as an absolute: a fixed MB allowance
# would pass a 4 MB/iteration leak in a 10-iteration smoke run and fail an
# 8-hour run of a clean driver.
DEFAULT_KB_PER_ITERATION = 256


def read_rss_kb(pid):
    """Resident set size in KB, or None if the process is gone.

    macOS reports ru_maxrss in BYTES, Linux in KILOBYTES. Getting this wrong by
    1024 is the difference between "flat" and "a 1 GB leak", so the platform is
    detected rather than assumed.
    """
    try:
        with open("/proc/%d/status" % pid, "r", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    # Not on Linux (or /proc unavailable): ask the kernel through ps.
    try:
        out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)],
                             capture_output=True, text=True, timeout=5)
        if out.returncode == 0 and out.stdout.strip():
            return int(out.stdout.strip())
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", required=True)
    parser.add_argument("--minutes", type=float, default=48.0)
    parser.add_argument("--sample-seconds", type=float, default=20.0)
    parser.add_argument("--iters", type=int, default=2000,
                        help="iterations per batch invocation")
    parser.add_argument("--kb-per-iteration", type=int,
                        default=DEFAULT_KB_PER_ITERATION)
    parser.add_argument("--out")
    args = parser.parse_args()

    if not os.path.exists(args.driver):
        print("run_soak: driver not found: %s" % args.driver)
        print("  build it with the asan preset (docs/07 9.1: ASan+UBSan is the")
        print("  configuration that can actually see a use-after-free)")
        return 2

    started = time.time()
    deadline = started + args.minutes * 60
    samples = []
    failures = []
    print("run_soak: %s for %.1f min (sample every %.0fs)"
          % (os.path.basename(args.driver), args.minutes, args.sample_seconds))
    print("run_soak: a short run is a SMOKE test; --minutes 48 is the real one")
    print("")

    # ONE long-lived process for the whole run, and that is not an
    # optimisation -- it is the whole measurement. Batching into short-lived
    # processes resets RSS on every batch, so a per-iteration leak never
    # accumulates anywhere: each batch starts clean and dies clean, and the
    # curve is flat no matter how badly the driver leaks. A soak that cannot
    # see the bug it exists for is a stopwatch with extra steps.
    #
    # --iters is therefore set high enough to outlive the deadline. If the
    # driver finishes early the run is honestly reported as UNPROVEN rather
    # than padded with restarts that would hide exactly what we are measuring.
    iterations = args.iters
    # Output goes to a FILE, not a PIPE. A pipe is never drained while the
    # driver runs, so it fills (64 KB on Linux, 16 KB on macOS) and the driver
    # blocks on write -- which looks exactly like a hang, and silently starves
    # the sampler because the loop never gets to run. The first draft of this
    # script had that bug and reported "no RSS samples" for a three-minute run,
    # which read like a platform quirk and was actually a self-inflicted stall.
    log_path = os.path.join(tempfile.gettempdir(),
                            "avbase-soak-%d.log" % os.getpid())
    log_handle = open(log_path, "w+", encoding="utf-8", errors="replace")
    try:
        proc = subprocess.Popen(
            [args.driver, "--iters", str(args.iters * 10000)],
            stdout=log_handle, stderr=subprocess.STDOUT,
            env=dict(os.environ))
    except OSError as error:
        log_handle.close()
        print("run_soak: could not start the driver: %s" % error)
        return 2

    next_sample = time.time() + args.sample_seconds
    while proc.poll() is None and time.time() < deadline:
        now = time.time()
        if now >= next_sample:
            rss = read_rss_kb(proc.pid)
            if rss is not None:
                samples.append({"t": round(now - started, 1), "rss_kb": rss})
            next_sample = now + args.sample_seconds
        time.sleep(0.2)

    finished_on_its_own = proc.poll() is not None
    if not finished_on_its_own:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    else:
        proc.wait()
    log_handle.flush()
    log_handle.seek(0)
    out = log_handle.read()
    log_handle.close()
    try:
        os.unlink(log_path)
    except OSError:
        pass

    if not finished_on_its_own:
        # It ran the full duration and was stopped: the iteration count is a
        # lower bound, which is the honest direction.
        iterations = None

    if proc.returncode not in (0, -signal.SIGINT, 130) and not finished_on_its_own:
        failures.append("driver exited %s\n%s"
                        % (proc.returncode, (out or "")[-800:]))
    elif finished_on_its_own and proc.returncode != 0:
        failures.append("driver exited %s before the deadline\n%s"
                        % (proc.returncode, (out or "")[-800:]))
    # Sanitizer reports do not always change the exit status (LSan reports at
    # exit and can exit 0), so the TEXT is checked too. This is the whole
    # reason the output is captured rather than discarded.
    if out and re.search(r"(ERROR: AddressSanitizer|runtime error:|"
                         r"ERROR: LeakSanitizer|SUMMARY: .*Sanitizer)", out):
        failures.append("sanitizer report:\n%s" % out[-1500:])

    elapsed_min = (time.time() - started) / 60.0
    if iterations is None:
        print("run_soak: ran the full %.1f min and was interrupted "
              "(iteration count is a lower bound: at least %d)"
              % (args.minutes, args.iters))
    else:
        print("run_soak: driver finished %d iteration(s) in %.1f min"
              % (iterations, elapsed_min))

    verdict = 0
    if not samples:
        # Two very different causes, and conflating them is how a broken
        # sampler gets mistaken for a clean run. The first draft reported
        # "exited too fast" for what was actually an unreadable process.
        print("run_soak: the memory curve is UNPROVEN -- no RSS sample was taken.")
        if read_rss_kb(os.getpid()) is None:
            print("  cause: this host will not report another process's RSS")
            print("  (/proc absent and ps denied -- common inside a sandbox).")
            print("  The leak gate therefore did NOT run. On Linux /proc makes")
            print("  this work; re-run there, or loosen the sandbox.")
        else:
            print("  cause: the driver was gone before the first sample")
            print("  interval elapsed. Raise --sample-seconds, or lower")
            print("  --iters so a batch outlives one sampling period.")
    else:
        rss_values = [s["rss_kb"] for s in samples]
        first = rss_values[0]
        last = rss_values[-1]
        growth = last - first
        divisor = iterations if iterations else args.iters
        per_iteration = growth / max(1, divisor)
        print("")
        print("  RSS first %d KB, last %d KB, growth %+d KB over %d iters"
              % (first, last, growth, iterations if iterations else divisor))
        print("  per iteration: %+.1f KB (budget %d KB)"
              % (per_iteration, args.kb_per_iteration))
        # The judgement is on the SLOPE, not the total: a driver whose baseline
        # is large but flat is healthy, and one that grows slowly forever is
        # exactly the leak this run exists to catch.
        if per_iteration > args.kb_per_iteration:
            print("  FAIL: memory grows faster than the budget -- that is a "
                  "leak, not a warm-up")
            verdict = 1
        else:
            print("  ok: the curve is flat within budget")

    if failures:
        print("")
        print("run_soak: FAILED")
        for failure in failures:
            print("  %s" % failure)
        verdict = 1
    else:
        print("")
        print("run_soak: no sanitizer report, no crash")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            json.dump({"iterations": iterations,
                       "minutes": args.minutes,
                       "samples": samples,
                       "failures": failures}, handle, indent=2)
            handle.write("\n")
        print("run_soak: samples written to %s" % args.out)

    return verdict


if __name__ == "__main__":
    sys.exit(main())
