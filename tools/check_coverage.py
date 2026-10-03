#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Enforce the coverage thresholds from docs/07 §12.

WHY THIS IS A SCRIPT AND NOT A CI YAML BLOCK. The thresholds in docs/07 §12 are
per-module and per-metric (line AND branch), with two modules held to 100% line
coverage because everything else depends on them being exhaustively tested. That
is too much state to keep in a shell one-liner, and a YAML block that nobody can
run locally is a gate that only CI can enforce -- which is how gates rot.

The previous state was `lcov --summary ... || true`, which cannot fail by
construction. This script is the difference between reporting a number and
holding a line.

USAGE
    tools/check_coverage.py --info <cov.info> [--baseline <file>] [--ratchet]

    --info      an lcov tracefile (what `lcov --capture` writes)
    --baseline  JSON of recorded per-module coverage, written after the run.
    --ratchet   enforce the recorded baseline as a FLOOR (fail on any
                regression) instead of the absolute docs/07 §12 thresholds.

TWO MODES, AND WHY THE SECOND ONE EXISTS
    Default (absolute) is the spec's executable form: it fails until every
    module reaches its docs/07 §12 number. That is the target state.

    --ratchet is how you get there without shipping a red pipeline. It holds
    today's measured numbers as the floor, so coverage can only go up, and
    prints how far each module still is from spec. It is a TEMPORARY mode: the
    moment a module reaches its docs/07 §12 number the ratchet is redundant
    for it, and the gate should be switched back to absolute.

    The reason this is not "just make the numbers smaller": the spec
    thresholds are still printed on every ratchet run and still enforced in
    absolute mode, so the gap stays visible instead of being normalised away.

EXIT CODES
    0  every threshold met
    1  at least one threshold missed (the reason is printed)
    2  the tracefile could not be read or contains no data

WHY THE THRESHOLDS ARE NOT NEGOTIABLE HERE. docs/07 §12 is the spec; this
script is its executable form. Making the numbers adjustable from the command
line would let a CI invocation quietly pass a build that should have failed,
which is the exact failure mode `|| true` had.
"""

import argparse
import json
import os
import re
import sys

# docs/07 §12. "core" excludes platform/sdl2 and platform/linux, which have no
# threshold because they are covered by contract + e2e + human acceptance.
#
# The two per-file keys carry the path the file ACTUALLY lives at. docs/07 §12
# spells them `media/filters/video_frame_compositor.cc` and
# `media/filters/av_sync_controller.cc`, but those paths do not exist in the
# tree -- the files were moved under `media/filters/legacy/` when the ffplay port
# was separated for LGPL provenance (see docs/archive/PROGRESS-rounds-1-9.md).
# Left as written, both rules matched nothing and the gate reported them as
# passing. UNMATCHED_THRESHOLDS below turns that class of mistake into a loud
# failure instead.
THRESHOLDS = {
    "base/": (95, 90),
    "media/base/": (95, 90),
    "media/filters/legacy/video_frame_compositor.cc": (100, 95),
    "media/filters/legacy/av_sync_controller.cc": (100, 95),
    "media/filters/": (90, 85),
    "player/": (90, 85),
    "media/filters/ffmpeg_": (70, None),
    "platform/null": (95, None),
}

CORE_OVERALL = (85, 80)

# WHAT IS COUNTED AND WHAT IS NOT.
#
# An earlier note here claimed the legacy/ subtree is exempt from thresholds
# because "reformatting or restructuring it to chase a number would damage the
# thing that makes it auditable". That reasoning is sound for code kept purely as
# provenance -- but it does not describe this subtree, and acting on it would
# have silently un-gated the two files docs/07 §12 marks as most critical.
# Checked rather than assumed: media/filters/legacy/ is reached from production
# code, not only from tests --
#     media/filters/video_renderer_impl.h   -> legacy/video_frame_compositor.h
#     media/filters/pipeline_impl.h         -> legacy/av_sync_controller.h
#     media/filters/renderer_impl.h         -> both
#     media/filters/audio_renderer_impl.h   -> legacy/av_sync_controller.h
#     media/renderers/default_renderer_factory.h -> both
# It is the live sync/display path that happens to carry LGPL-2.1 provenance.
# So legacy/ is counted like any other production directory, and the two files
# docs/07 §12 singles out get their own stricter per-file rules above.
#
# What IS excluded: test code, vendored code, and the two platforms docs/07 §12
# deliberately leaves to contract + e2e + human acceptance.
EXCLUDED_PREFIXES = ("tests/", "third_party/", "platform/sdl2/", "platform/linux/")

#  Coverage is computed only over instrumented code, so a file nobody built
#  shows up as absent rather than as zero. Treating absent as zero would make
#  the gate fail for a module the preset did not compile.
FILE_RE = re.compile(r"^SF:(.*)$")


class ModuleCoverage:
    def __init__(self):
        self.found = 0
        self.hit = 0
        self.br_found = 0
        self.br_hit = 0
        self.files = 0

    def line_pct(self):
        return 100.0 if self.found == 0 else 100.0 * self.hit / self.found

    def branch_pct(self):
        if self.br_found == 0:
            return None
        return 100.0 * self.br_hit / self.br_found

    def merge(self, other):
        self.found += other.found
        self.hit += other.hit
        self.br_found += other.br_found
        self.br_hit += other.br_hit
        self.files += 1


def parse_lcov(path):
    """Returns {source_path: ModuleCoverage} from an lcov tracefile.

    Written against the tracefile grammar rather than shelling out to lcov so the
    gate runs anywhere python3 does -- including a macOS laptop with no lcov,
    which is where a threshold is most often re-argued.
    """
    modules = {}
    current = None
    cov = None
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            match = FILE_RE.match(line)
            if match:
                current = match.group(1)
                cov = ModuleCoverage()
                continue
            if current is None:
                continue
            if line.startswith("LF:"):
                cov.found = int(line.split(":", 1)[1])
            elif line.startswith("LH:"):
                cov.hit = int(line.split(":", 1)[1])
            elif line.startswith("BRF:"):
                cov.br_found = int(line.split(":", 1)[1])
            elif line.startswith("BRH:"):
                cov.br_hit = int(line.split(":", 1)[1])
            elif line == "end_of_record":
                modules[current] = cov
                current = None
                cov = None
    return modules


def normalize(path, root):
    """Make a tracefile path comparable to a THRESHOLDS prefix.

    lcov records absolute paths; the thresholds are repo-relative. Comparing
    raw strings is how a coverage gate ends up silently matching nothing and
    passing forever.
    """
    path = path.replace("\\", "/")
    root = root.replace("\\", "/").rstrip("/")
    if path.startswith(root):
        path = path[len(root):]
    # Strip a leading source root the tracefile may carry, e.g. ".../ijkpp/".
    for marker in ("/ijkpp/", "/avbase/"):
        index = path.find(marker)
        if index >= 0:
            return path[index + len(marker):]
    return path.lstrip("/")


def is_excluded(rel):
    return any(rel.startswith(prefix) for prefix in EXCLUDED_PREFIXES)


def collect(modules, root):
    """Buckets every counted file under the thresholds that apply to it.

    The most specific prefix wins: a file under media/filters/ matches both
    "media/filters/" and the per-file compositor rule, and the per-file rule is
    the one that must decide.
    """
    buckets = {key: ModuleCoverage() for key in THRESHOLDS}
    core = ModuleCoverage()
    # Derived, never restated: a per-file rule is whatever THRESHOLDS key names
    # a file. Keeping a second hand-written list here is how the two drifted
    # apart, which is what let both rules go unenforced.
    per_file = {key: ModuleCoverage()
                for key in THRESHOLDS if key.endswith(".cc")}
    for path, cov in modules.items():
        rel = normalize(path, root)
        if is_excluded(rel):
            continue
        core.merge(cov)
        if rel in per_file:
            per_file[rel].merge(cov)
            continue
        best = None
        for prefix in THRESHOLDS:
            if rel.startswith(prefix) and not prefix.endswith(".cc"):
                if best is None or len(prefix) > len(best):
                    best = prefix
        if best is not None:
            buckets[best].merge(cov)
    return buckets, per_file, core


def _fmt(value):
    return "n/a" if value is None else "%g" % value


def _lookup(previous, label):
    """Reads one entry out of a baseline snapshot.

    |label| carries the "* " display prefix for per-file rules, so it is
    stripped before the lookup -- otherwise every per-file rule would miss its
    own baseline and be reported as new, silently disabling the ratchet for
    exactly the two files docs/07 §12 cares most about.
    """
    if not previous:
        return None
    key = label[2:] if label.startswith("* ") else label
    if key.endswith(".cc"):
        entry = (previous.get("per_file") or {}).get(key)
    else:
        entry = (previous.get("modules") or {}).get(key)
    if entry is None:
        return None
    if isinstance(entry, dict):
        return entry
    # An older snapshot recorded a bare float for the line percentage.
    return {"line": entry, "branch": None}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--info", required=True, help="lcov tracefile")
    parser.add_argument("--root", default=os.getcwd())
    parser.add_argument("--baseline")
    parser.add_argument("--ratchet", action="store_true",
                        help="enforce the recorded baseline as a floor instead "
                             "of the docs/07 §12 absolute thresholds")
    args = parser.parse_args()

    if not os.path.exists(args.info):
        print("check_coverage: tracefile not found: %s" % args.info)
        return 2
    modules = parse_lcov(args.info)
    if not modules:
        print("check_coverage: tracefile has no records: %s" % args.info)
        print("  (a coverage gate that reads an empty file and passes is worse "
              "than no gate -- it reports success without measuring)")
        return 2

    buckets, per_file, core = collect(modules, args.root)

    failures = []
    warnings = []
    print("check_coverage: docs/07 section 12 thresholds")
    print("")

    # One flat list of everything judged, so the two modes below differ only in
    # the comparison they apply and not in what they walk. Absolute mode asks
    # "did we reach the spec"; ratchet mode asks "did we lose ground".
    judged = []
    ordered = sorted(THRESHOLDS.items(), key=lambda kv: -len(kv[0]))
    for prefix, (min_line, min_branch) in ordered:
        if prefix.endswith(".cc"):
            continue          # per-file rule; reported in its own section below
        cov = buckets[prefix]
        if cov.files == 0:
            # A prefix that matches nothing is a gap in MEASUREMENT, not a pass.
            # Reporting it as "ok" is how a gate ends up enforcing nothing.
            warnings.append(
                "%s: no instrumented file under this prefix in this build -- "
                "threshold NOT checked" % prefix)
            continue
        judged.append((prefix, cov, min_line, min_branch))

    for path, cov in per_file.items():
        if cov.files == 0:
            # This is the failure mode the THRESHOLDS comment warns about.
        # A named file that never appears is either a stale path or a file
        # the preset never built; either way the rule is not being held, and
        # saying nothing about it is the one thing that must not happen.
            failures.append(
                "%s: per-file threshold matched no instrumented file -- the "
                "path in docs/07 §12 is stale, or the coverage preset does not "
                "build it" % path)
            continue
        min_line, min_branch = THRESHOLDS[path]
        judged.append(("* " + path, cov, min_line, min_branch))

    previous = None
    if args.baseline and os.path.exists(args.baseline):
        try:
            with open(args.baseline, "r", encoding="utf-8") as handle:
                previous = json.load(handle)
        except (ValueError, OSError):
            previous = None

    for label, cov, min_line, min_branch in judged:
        line = cov.line_pct()
        branch_text = "  n/a"
        branch = None
        if cov.br_found:
            branch = cov.branch_pct()
            branch_text = "%6.1f%%" % branch

        status = "ok"
        gap = ""
        if args.ratchet:
            before = _lookup(previous, label)
            if before is None:
                # Not recorded yet: today's number becomes the floor. Failing
                # here would mean a new module can never appear.
                gap = "  (new: recorded as the floor)"
            else:
                if line + 1e-9 < (before or {}).get("line", line):
                    status = "FAIL"
                    failures.append(
                        "%s: line %.1f%% regressed from %.1f%%"
                        % (label, line, before["line"]))
                if (branch is not None and before
                        and before.get("branch") is not None
                        and branch + 1e-9 < before["branch"]):
                    status = "FAIL"
                    failures.append(
                        "%s: branch %.1f%% regressed from %.1f%%"
                        % (label, branch, before["branch"]))
                if before:
                    gap = "  (spec: line >=%g, branch >=%s)"
                    gap = gap % (min_line, _fmt(min_branch))
        else:
            if line + 1e-9 < min_line:
                status = "FAIL"
                failures.append("%s: line %.1f%% < %.1f%%"
                                % (label, line, min_line))
            if branch is not None and min_branch is not None:
                if branch + 1e-9 < min_branch:
                    status = "FAIL"
                    failures.append("%s: branch %.1f%% < %.1f%%"
                                    % (label, branch, min_branch))

        print("  %-42s line %6.1f%% (>=%d)  branch %s   %s%s"
              % (label, line, min_line, branch_text, status, gap))

    core_line = core.line_pct()
    core_branch = core.branch_pct() if core.br_found else None
    core_branch_text = "%6.1f%%" % core_branch if core.br_found else "  n/a"
    core_status = "ok"
    core_gap = ""
    if args.ratchet:
        before = (previous or {}).get("core")
        if before is None:
            core_gap = "  (new: recorded as the floor)"
        else:
            if core_line + 1e-9 < before.get("line", core_line):
                core_status = "FAIL"
                failures.append("core overall: line %.1f%% regressed from %.1f%%"
                                % (core_line, before["line"]))
            if (core_branch is not None and before.get("branch") is not None
                    and core_branch + 1e-9 < before["branch"]):
                core_status = "FAIL"
                failures.append("core overall: branch %.1f%% regressed from %.1f%%"
                                % (core_branch, before["branch"]))
            core_gap = "  (spec: line >=%g, branch >=%g)" % CORE_OVERALL
    else:
        if core_line + 1e-9 < CORE_OVERALL[0]:
            core_status = "FAIL"
            failures.append("core overall: line %.1f%% < %.1f%%"
                            % (core_line, CORE_OVERALL[0]))
        if core_branch is not None and core_branch + 1e-9 < CORE_OVERALL[1]:
            core_status = "FAIL"
            failures.append("core overall: branch %.1f%% < %.1f%%"
                            % (core_branch, CORE_OVERALL[1]))
    print("  %-42s line %6.1f%% (>=%d)  branch %s   %s%s"
          % ("CORE OVERALL (excl. sdl2/linux)", core_line, CORE_OVERALL[0],
             core_branch_text, core_status, core_gap))

    if args.baseline:
        # Both metrics are recorded, not just line: a ratchet that only watches
        # lines would let branch coverage fall away unnoticed, and branch is
        # where the sync and error paths live.
        snapshot = {
            "core": {"line": core_line, "branch": core_branch},
            "modules": {
                k: {"line": v.line_pct(), "branch": v.branch_pct()}
                for k, v in buckets.items() if v.files},
            "per_file": {
                k: {"line": v.line_pct(), "branch": v.branch_pct()}
                for k, v in per_file.items() if v.files},
        }
        try:
            with open(args.baseline, "w", encoding="utf-8") as handle:
                json.dump(snapshot, handle, indent=2, sort_keys=True)
                handle.write("\n")
        except OSError as error:
            print("check_coverage: could not write baseline: %s" % error)

    if warnings:
        print("")
        print("check_coverage: %d threshold(s) NOT CHECKED" % len(warnings))
        for warning in warnings:
            print("  ! %s" % warning)

    print("")
    if failures:
        print("check_coverage: FAILED")
        for failure in failures:
            print("  - %s" % failure)
        print("")
        print("  These are docs/07 section 12 thresholds, not preferences. If a")
        print("  module genuinely cannot meet one, the fix is a test or a")
        print("  deliberate change to the spec -- not a smaller number here.")
        return 1
    print("check_coverage: all thresholds met")
    return 0


if __name__ == "__main__":
    sys.exit(main())
