#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Turn gcov's .gcov output into an lcov tracefile.

WHAT THIS IS FOR
    docs/12 4.6 wants a coverage GATE, and the gate reads an lcov tracefile.
    On Linux CI that is one command (`lcov --capture`). On macOS there is no
    lcov, and Apple clang's gcov does not emit that format: `gcov -i` produces
    an intermediate format, and plain `gcov` writes .gcov TEXT FILES next to the
    .gcda. This script is the missing step, so the gate has ONE input format
    instead of two code paths that can disagree.

THREE THINGS THAT ARE EASY TO GET WRONG -- all three were wrong in v1
    1. gcov does not print coverage to stdout. stdout carries only a summary
       ("File '...' / Lines executed:81.25% of 48 / Creating 'x.cc.gcov'");
       the per-line data is in the .gcov FILE. v1 parsed stdout, so its
       `Source:` header regex never matched, every record was dropped, and it
       reported "0 file(s), 0 instrumented line(s)". That output is
       indistinguishable from "the code has no coverage", which is why it
       survived a read.
    2. One .gcda yields one .gcov per file in its translation unit -- the .cc
       plus every header it includes, most of which are libc++ headers under
       the SDK. A measured 2-file run produced 97 and 103 such files. Those are
       dropped unless the `Source:` path is under --source, otherwise the SDK's
       coverage is averaged into the project's.
    3. Because of (2), the same source file is reported by MANY .gcda runs.
       Appending them would count a header's lines once per including TU.
       Records are therefore MERGED by source path: line counts summed, branch
       outcomes OR-ed, function counts summed.

WHAT IS EXACT AND WHAT IS NOT
    Lines      exact.  A number is a count; `#####` / `=====` means zero.
    Functions  exact counts, from `function NAME called N ...`.
    Branches   approximate. gcov says "branch 0 taken 71%" and never says how
               often the branch ran, so BRDA carries `-` or `1`: WHETHER an
               outcome was ever exercised is exact, HOW OFTEN is invented.
               Enough to gate on ("did branch coverage regress"), not enough to
               publish as a metric. docs/07 section 12 states its thresholds in
               LINE coverage, so the approximate part is not the gated part.

USAGE
    tools/gcov_to_lcov.py --root <build-dir> --source <repo-root> -o cov.info
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

# "        -:    0:Source:/repo/media/ffmpeg/log_bridge.cc"
SOURCE_RE = re.compile(r"^\s*-\s*:\s*0:Source:(.*)$")

# "    21188:   15:  code"  |  "    #####:   12:  code"  |  "        -:   14:"
# The marker class is deliberately loose: gcov has spelled "not executed" as
# #####, ===== and $$$$$ in different versions, and an unrecognised spelling
# should read as zero rather than silently disappear from the denominator.
LINE_RE = re.compile(r"^\s*([^\s:]+):\s*([0-9]+):(.*)$")

# "branch  0 taken 71%"  |  "branch  0 never executed"
BRANCH_TAKEN_RE = re.compile(r"^branch\s+(\d+)\s+taken\s+(\d+)%")
BRANCH_NEVER_RE = re.compile(r"^branch\s+(\d+)\s+never\s+executed")

# "function _ZN6avbase8platform6ffmpeg13MapAvLogLevelEi called 70432 ..."
FUNCTION_RE = re.compile(r"^function\s+(\S+)\s+called\s+(\d+)")


class FileCoverage(object):
    """Coverage for one source file, merged across every TU that reports it."""

    def __init__(self, path):
        self.path = path
        self.line_counts = {}      # lineno -> summed count
        self.branch_taken = {}     # (lineno, branch_id) -> ever taken
        self.function_lines = {}   # mangled name -> starting line
        self.function_counts = {}  # mangled name -> summed count

    def add_line(self, lineno, count):
        self.line_counts[lineno] = self.line_counts.get(lineno, 0) + count

    def add_branch(self, lineno, branch_id, taken):
        key = (lineno, branch_id)
        self.branch_taken[key] = self.branch_taken.get(key, False) or taken

    def add_function(self, name, lineno, count):
        self.function_counts[name] = self.function_counts.get(name, 0) + count
        # A function in a header is reported once per including TU, always with
        # the same starting line, so the first writer is as good as any.
        self.function_lines.setdefault(name, lineno)

    def write(self, out):
        out.write("SF:%s\n" % self.path)

        for name in sorted(self.function_lines):
            out.write("FN:%d,%s\n" % (self.function_lines[name], name))
        for name in sorted(self.function_counts):
            out.write("FNDA:%d,%s\n" % (self.function_counts[name], name))
        hit_functions = sum(1 for c in self.function_counts.values() if c)
        out.write("FNF:%d\n" % len(self.function_counts))
        out.write("FNH:%d\n" % hit_functions)

        for (lineno, branch_id) in sorted(self.branch_taken):
            taken = self.branch_taken[(lineno, branch_id)]
            out.write("BRDA:%d,0,%d,%s\n" % (lineno, branch_id,
                                            "1" if taken else "-"))
        hit_branches = sum(1 for t in self.branch_taken.values() if t)
        out.write("BRF:%d\n" % len(self.branch_taken))
        out.write("BRH:%d\n" % hit_branches)

        for lineno in sorted(self.line_counts):
            out.write("DA:%d,%d\n" % (lineno, self.line_counts[lineno]))
        hit_lines = sum(1 for c in self.line_counts.values() if c)
        out.write("LF:%d\n" % len(self.line_counts))
        out.write("LH:%d\n" % hit_lines)
        out.write("end_of_record\n")


def parse_gcov(text):
    """Parses one .gcov file into (source, lines, branches, functions).

    Branch and function markers occupy the lines BEFORE the source line they
    describe, so they are buffered and attached to the next source line that
    carries a count. A marker followed by a non-executable line is dropped
    rather than attached to the wrong line.
    """
    source = None
    lines = []
    branches = []
    functions = []
    pending_branches = []
    pending_functions = []

    for raw in text.split("\n"):
        match = SOURCE_RE.match(raw)
        if match:
            source = match.group(1).strip()
            continue

        match = BRANCH_TAKEN_RE.match(raw)
        if match:
            pending_branches.append((int(match.group(1)), int(match.group(2)) > 0))
            continue

        match = BRANCH_NEVER_RE.match(raw)
        if match:
            pending_branches.append((int(match.group(1)), False))
            continue

        match = FUNCTION_RE.match(raw)
        if match:
            pending_functions.append((match.group(1), int(match.group(2))))
            continue

        match = LINE_RE.match(raw)
        if not match:
            continue
        marker, lineno = match.group(1), int(match.group(2))

        if marker == "-":
            pending_branches = []
            pending_functions = []
            continue

        count = int(marker) if marker.isdigit() else 0
        lines.append((lineno, count))
        for branch_id, taken in pending_branches:
            branches.append((lineno, branch_id, taken))
        for name, fn_count in pending_functions:
            functions.append((name, lineno, fn_count))
        pending_branches = []
        pending_functions = []

    return source, lines, branches, functions


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, help="build directory")
    parser.add_argument("--source", required=True, help="repository root")
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--exclude", default=None,
                        help="regex; source paths matching it are dropped")
    parser.add_argument("--progress", action="store_true",
                        help="print per-.gcda timing, to spot outliers")
    args = parser.parse_args()

    source_root = os.path.realpath(args.source)

    gcda_files = []
    for dirpath, _dirnames, filenames in os.walk(args.root):
        for name in filenames:
            if name.endswith(".gcda"):
                gcda_files.append(os.path.join(dirpath, name))
    if not gcda_files:
        sys.stderr.write("gcov_to_lcov: no .gcda under %s -- run the tests first\n"
                         % args.root)
        return 2
    gcda_files.sort()

    exclude = re.compile(args.exclude) if args.exclude else None
    merged = {}
    dropped_outside = 0
    failures = []
    timings = []

    # gcov writes .gcov files into the CWD, named after each source file, with
    # no way to redirect them. Two .gcda whose sources share a basename would
    # therefore clobber each other in a shared directory, so each gets its own
    # scratch dir -- and the scratch root lives outside the build tree so the
    # intermediates can never leak into the repository.
    scratch_root = tempfile.mkdtemp(prefix="gcov_to_lcov_")
    try:
        for index, gcda in enumerate(gcda_files):
            workdir = os.path.join(scratch_root, "%05d" % index)
            os.mkdir(workdir)
            started = time.monotonic()
            result = subprocess.run(["gcov", "-b", os.path.abspath(gcda)],
                                    cwd=workdir, capture_output=True, text=True)
            timings.append((time.monotonic() - started, gcda))

            if result.returncode != 0:
                detail = (result.stderr or "").strip().splitlines()
                failures.append((gcda, result.returncode,
                                 detail[0] if detail else ""))
                continue

            for name in sorted(os.listdir(workdir)):
                if not name.endswith(".gcov"):
                    continue
                with open(os.path.join(workdir, name), "r",
                          encoding="utf-8", errors="replace") as handle:
                    text = handle.read()

                source, lines, branches, functions = parse_gcov(text)
                if source is None:
                    continue

                real = os.path.realpath(source)
                if not (real == source_root or real.startswith(source_root + os.sep)):
                    dropped_outside += 1
                    continue
                if exclude is not None and exclude.search(real):
                    dropped_outside += 1
                    continue

                entry = merged.get(real)
                if entry is None:
                    entry = FileCoverage(real)
                    merged[real] = entry
                for lineno, count in lines:
                    entry.add_line(lineno, count)
                for lineno, branch_id, taken in branches:
                    entry.add_branch(lineno, branch_id, taken)
                for fn_name, lineno, count in functions:
                    entry.add_function(fn_name, lineno, count)

            shutil.rmtree(workdir, ignore_errors=True)

            if args.progress:
                sys.stderr.write("[%3d/%d] %6.0f ms  %s\n"
                                 % (index + 1, len(gcda_files),
                                    timings[-1][0] * 1000.0,
                                    os.path.basename(gcda)))
    finally:
        shutil.rmtree(scratch_root, ignore_errors=True)

    with open(args.output, "w", encoding="utf-8") as out:
        for path in sorted(merged):
            merged[path].write(out)

    total_lines = sum(len(e.line_counts) for e in merged.values())
    total_hit = sum(sum(1 for c in e.line_counts.values() if c)
                    for e in merged.values())
    total_branches = sum(len(e.branch_taken) for e in merged.values())
    total_branches_hit = sum(sum(1 for t in e.branch_taken.values() if t)
                             for e in merged.values())

    print("gcov_to_lcov: %d/%d .gcda -> %d source file(s) -> %s"
          % (len(gcda_files) - len(failures), len(gcda_files),
             len(merged), args.output))
    print("gcov_to_lcov: %d file(s) outside %s dropped (SDK / system headers)"
          % (dropped_outside, source_root))
    if total_lines:
        print("gcov_to_lcov: lines   %6.2f%% (%d/%d)"
              % (100.0 * total_hit / total_lines, total_hit, total_lines))
    if total_branches:
        print("gcov_to_lcov: branches %5.2f%% (%d/%d)  [APPROXIMATE]"
              % (100.0 * total_branches_hit / total_branches,
                 total_branches_hit, total_branches))
    print("gcov_to_lcov: NOTE branch counts are hit/miss only -- gcov reports "
          "a percentage, not a count")

    if failures:
        sys.stderr.write("gcov_to_lcov: %d .gcda failed:\n" % len(failures))
        for path, code, why in failures[:10]:
            sys.stderr.write("  exit %d  %s  %s\n" % (code, path, why))
        return 1
    if total_lines == 0:
        sys.stderr.write("gcov_to_lcov: parsed 0 instrumented lines -- the "
                         "parser is probably pointed at the wrong input\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
