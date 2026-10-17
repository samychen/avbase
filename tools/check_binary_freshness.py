#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Fails when a build tree holds an executable older than a library it links.

WHY THIS EXISTS
Every measurement in this repo is made by running something out of a build
tree. If the binary predates the library it was linked against, the run
reports on code that no longer exists -- and it keeps reporting the old
answer, confidently, as often as you like. That is the failure mode this
script exists to make loud:

  * a ctest run reporting "N/N passed" against a stale binary is reporting on
    last week's build, and its test COUNT is the tell: a tree that gained
    tests shows the old, smaller number;
  * an example that "cannot complete playback" may simply have been linked
    before the fix landed.

WHAT ACTUALLY HAPPENS (it is not a ninja bug)
Ninja behaves correctly here. It did what it was asked. The incidents come
from building PART of the tree: `cmake --build --target libavbase_media.a`,
or a build preset narrowed with `targets:`, rebuilds the library and leaves
every executable that links it untouched -- and the build reports success.
The library is newer than the binary, and running the binary exercises the
old code.

Exit codes match the other checkers in tools/ (see check_protocols.py):
  0  every comparable executable is at least as new as the libraries it links
  1  at least one executable is stale (listed on stdout)
  2  nothing could be compared -- no ninja build tree under --root, or the
     tree(s) found carry no readable .ninja_log. A tree we cannot parse
     cannot certify freshness, so this is a failure, not a skip.

HOW STALENESS IS DECIDED
From .ninja_log, not from stat(). Ninja records the output mtime it wrote
for each edge, and comparing log-to-log sidesteps everything the filesystem
does between edges (CMake's archive rule ends in `cmake -E touch`, so an
archive's mtime advances on every relink; note the recorded value is
nanosecond-precision and is not the same number as stat().st_mtime_ns).
The dependency edges come from build.ninja, so each executable is compared
only against the libraries it actually links.

WHAT COUNTS AS A COMPARISON
An executable built in this tree, against a library from this tree that
ninja also built here. Both numbers are reported at the end: a run that
compares nothing is exit 2, never a green "everything is fresh".
"""

import argparse
import os
import sys

# Pruned while walking, so `--root .` does not spend its time inside vendored
# dependency trees (tools/deps is half a gigabyte). Deliberately NOT the set
# tools/check_invariants.py uses: that checker prunes "build*" because it wants
# source files, while this one is looking FOR build trees. Only the one
# throwaway tree is named explicitly -- tools/build/ holds the one-shot
# verification trees left by tools/setup_glfw.sh and friends, and nobody
# measures anything there.
PRUNED_DIRS = frozenset([".git", "docs", "deps", "node_modules", "third_party"])
# tools/build/ and everything under it -- the one-shot verification trees
# tools/setup_glfw.sh and friends leave behind. Nobody measures those, and
# their binaries are routinely stale. Compared root-relative so that
# `--root .` and `--root /abs/path` both prune it.
PRUNED_RELATIVE = frozenset([os.path.join("tools", "build")])


def parse_ninja_log(path):
    """Returns (output path -> mtime, count of unreadable rows).

    .ninja_log rows are: start_ms end_ms mtime output command_hash. Rewrites
    append rows, so the last row per output is the current one; we compare
    mtimes rather than trusting row order. The leading "# ninja log vN" line
    is not a row and is skipped for that reason, not by accident.

    An unreadable row is COUNTED rather than dropped. A log we cannot fully
    parse must not be allowed to certify freshness: silently skipping the one
    broken row that happened to be a library turns a stale binary into a
    green run.
    """
    newest = {}
    unreadable = 0
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            try:
                mtime = int(parts[2])
            except (IndexError, ValueError):
                unreadable += 1
                continue
            if len(parts) != 5:
                unreadable += 1
                continue
            output = parts[3]
            if output not in newest or mtime > newest[output]:
                newest[output] = mtime
    return newest, unreadable


def parse_bin_edges(build_ninja):
    """Returns (bin/ executable -> the lib/ inputs ninja links it against).

    Ninja's edge line is
        build <outs...>: <rule> <explicit ins> | <implicit ins> || <order-only>

    Three things about that grammar are load-bearing here:

    * <outs...> may list SEVERAL outputs on one edge. CMake uses this for the
      test binaries: `build bin/base_unittests tests/base_unittests[1]_tests.cmake`
      links the unittest and emits ctest's registration file together. A
      parser that assumes one output per line drops every unittest binary in
      the tree -- silently, because a dropped edge is indistinguishable from a
      fresh one. Every token on the left counts.
    * The libraries live in the IMPLICIT section (after the single `|`), not
      in the explicit inputs, which are the object files.
    * The order-only section (after `||`) repeats library paths purely for
      build ordering. Including it would make every consumer look dependent
      on libraries it merely has to be built after.
    """
    edges = {}
    with open(build_ninja, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not line.startswith("build bin/"):
                continue
            outputs, colon, rest = line[len("build "):].partition(":")
            if not colon:
                continue
            implicit = rest.partition("||")[0].partition("|")[2]
            libraries = [
                token for token in implicit.split() if token.startswith("lib/")
            ]
            for output in outputs.split():
                if output.startswith("bin/"):
                    edges[output] = libraries
    return edges


def find_build_dirs(root):
    """Every directory under root that holds a configured ninja build tree."""
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [
            d for d in dirnames
            if d not in PRUNED_DIRS
            and os.path.relpath(os.path.join(dirpath, d), root)
            not in PRUNED_RELATIVE
        ]
        if "build.ninja" in filenames:
            found.append(dirpath)
    return sorted(found)


def inspect_build_dir(build_dir):
    """Returns (stale descriptions, comparisons made, unusable reason).

    Exactly one of "stale descriptions" and "unusable reason" is meaningful;
    unusable_reason is None for a tree this checker can judge.
    """
    log = os.path.join(build_dir, ".ninja_log")
    ninja = os.path.join(build_dir, "build.ninja")
    if not os.path.isfile(log):
        return None, 0, (
            "%s has a build.ninja but no .ninja_log -- it was configured but "
            "never built here" % build_dir
        )

    try:
        outputs, unreadable = parse_ninja_log(log)
        edges = parse_bin_edges(ninja)
    except OSError as exc:
        return None, 0, "%s cannot be read (%s)" % (build_dir, exc)

    if unreadable:
        return None, 0, (
            "%s: %d unreadable row(s) in .ninja_log -- a log that does not "
            "parse cannot certify freshness" % (build_dir, unreadable)
        )
    if not outputs:
        return None, 0, "%s has an empty .ninja_log" % build_dir

    stale = []
    compared = 0
    for binary, inputs in sorted(edges.items()):
        binary_mtime = outputs.get(binary)
        if binary_mtime is None:
            continue  # ninja has never built it here; nothing to compare
        linked = [(outputs[path], path) for path in inputs if path in outputs]
        if not linked:
            continue  # links nothing this tree built
        compared += 1
        newest_input, newest_input_path = max(linked)
        if binary_mtime < newest_input:
            stale.append(
                "  %s is older than %s (the library changed after the last "
                "link; anything you measure with this binary describes old "
                "code)" % (binary, newest_input_path)
            )
    return stale, compared, None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        default=".",
        help="directory to search for build trees (default: the current one)",
    )
    args = parser.parse_args()

    build_dirs = find_build_dirs(args.root)
    if not build_dirs:
        print("check_binary_freshness: no ninja build tree under %s" % args.root)
        return 2

    compared = 0
    stale_trees = 0
    total_stale = 0
    unusable = []
    for build_dir in build_dirs:
        stale, count, reason = inspect_build_dir(build_dir)
        if reason is not None:
            unusable.append(reason)
            continue
        compared += count
        if stale:
            stale_trees += 1
            total_stale += len(stale)
            print("check_binary_freshness: %s" % build_dir)
            for line in stale:
                print(line)
            print(
                "  fix: you built part of the tree. Rebuild the targets you "
                "are about to measure -- `cmake --build <dir> --target "
                "<name>` or drop the `targets:` narrowing from the preset."
            )

    for reason in unusable:
        print("check_binary_freshness: %s" % reason)

    if total_stale:
        print(
            "check_binary_freshness: FAILED -- %d stale executable(s) across "
            "%d build tree(s)" % (total_stale, stale_trees)
        )
        return 1

    if unusable or not compared:
        print(
            "check_binary_freshness: FAILED -- 0 executable(s) compared "
            "(%d tree(s) inspected, %d unusable). Nothing was measured, so "
            "nothing is certified fresh." % (len(build_dirs), len(unusable))
        )
        return 2

    print(
        "check_binary_freshness: %d executable(s) compared across %d build "
        "tree(s), each at least as new as the libraries it links"
        % (compared, len(build_dirs) - len(unusable))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
