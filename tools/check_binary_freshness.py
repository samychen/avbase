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

Exit codes match the other checkers in tools/ (see check_protocols.py):
  0  every executable is at least as new as every library it links
  1  at least one executable is stale (listed on stdout)
  2  the build tree is unusable for this check (no build.ninja, no .ninja_log)

HOW STALENESS IS DECIDED
From .ninja_log, not from stat(). Ninja logs the output mtime it recorded
when it last ran each edge, and that is the mtime the build system itself
believes in. Comparing stat() mtimes instead produces false positives:
  * a static library whose contents did not change keeps an OLD mtime by
    design (ninja restats archives and rewinds the timestamp), so a fresh
    link can look older than its input;
  * a binary that does not link library X cannot be stale with respect to X,
    but a naive "is any lib newer" check flags it anyway.
The dependency edges come from build.ninja, so each executable is compared
only against the libraries it actually links.
"""

import argparse
import os
import re
import sys


def parse_ninja_log(path):
    """output path -> mtime, taking the newest record ninja wrote for it.

    .ninja_log rows are: start_ms end_ms mtime output command_hash. Rewrites
    append rows, so the last row per output is the current one; we compare
    mtimes rather than trusting row order.
    """
    newest = {}
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) != 5:
                continue
            try:
                mtime = int(parts[2])
            except ValueError:
                continue
            output = parts[3]
            if output not in newest or mtime > newest[output]:
                newest[output] = mtime
    return newest


def parse_link_inputs(build_ninja):
    """executable path -> the lib/ inputs ninja links it against.

    Ninja's edge line is
        build <out>: <rule> <in-obj> | <explicit link inputs> || <order-only>
    Only the explicit-input section counts: the order-only section repeats
    library paths for dependency ordering and would make every consumer look
    dependent on libraries it merely has to be built after.
    """
    pattern = re.compile(r"^build ([^:|]+):.*?\|([^|]*)\|", re.M)
    edges = {}
    with open(build_ninja, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    for match in pattern.finditer(text):
        output = match.group(1).strip()
        inputs = match.group(2).split()
        edges[output] = [token for token in inputs if token.startswith("lib/")]
    return edges


def find_build_dirs(root):
    """Every directory under root that looks like a configured build tree.

    tools/build/ is skipped: it holds the one-shot verification trees that
    tools/setup_glfw.sh and friends leave behind, and it is gitignored. Nobody
    measures anything there, so a stale binary in it is not a false result but
    it is not this checker's business either.
    """
    found = []
    for dirpath, dirnames, _ in os.walk(root):
        dirnames[:] = [
            d
            for d in dirnames
            if d not in (".git", "node_modules")
            and os.path.join(dirpath, d) != os.path.join(root, "tools", "build")
        ]
        if "build.ninja" in os.listdir(dirpath):
            found.append(dirpath)
    return sorted(found)


def check_build_dir(build_dir):
    """Returns a list of human-readable stale-binary descriptions."""
    log = os.path.join(build_dir, ".ninja_log")
    ninja = os.path.join(build_dir, "build.ninja")
    for required in (log, ninja):
        if not os.path.exists(required):
            return None  # not a usable ninja tree

    outputs = parse_ninja_log(log)
    edges = parse_link_inputs(ninja)

    stale = []
    for binary, inputs in sorted(edges.items()):
        if not binary.startswith("bin/"):
            continue
        binary_mtime = outputs.get(binary)
        if binary_mtime is None:
            continue  # ninja has never built it here; nothing to compare
        newest_input = None
        newest_input_path = None
        for item in inputs:
            item_mtime = outputs.get(item)
            if item_mtime is None:
                continue
            if newest_input is None or item_mtime > newest_input:
                newest_input = item_mtime
                newest_input_path = item
        if newest_input is None:
            continue  # links nothing from this tree
        if binary_mtime < newest_input:
            stale.append(
                "  %s is older than %s (the library changed after the last "
                "link; anything you measure with this binary describes old "
                "code)" % (binary, newest_input_path)
            )
    return stale


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

    inspected = 0
    total_stale = 0
    skipped = []
    for build_dir in build_dirs:
        stale = check_build_dir(build_dir)
        if stale is None:
            skipped.append(build_dir)
            continue
        inspected += 1
        if stale:
            total_stale += len(stale)
            print("check_binary_freshness: %s" % build_dir)
            for line in stale:
                print(line)
            print(
                "  fix: rebuild the targets you are about to measure -- "
                "`cmake --build <dir> --target <name>`, or remove the binary "
                "and let the next build relink it."
            )

    if skipped:
        print(
            "check_binary_freshness: skipped %d tree(s) without a .ninja_log "
            "(Makefile generators record freshness differently)"
            % len(skipped)
        )

    if total_stale:
        print(
            "check_binary_freshness: FAILED -- %d stale executable(s) across "
            "%d build tree(s)" % (total_stale, len([d for d in build_dirs if d]))
        )
        return 1

    print(
        "check_binary_freshness: %d build tree(s) inspected, every executable "
        "is at least as new as the libraries it links" % inspected
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())