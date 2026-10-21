#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Fails when a build tree holds an executable that does not describe the
current sources.

WHY THIS EXISTS
Every measurement in this repo is made by running something out of a build
tree. If the binary predates the code it was built from, the run reports on
code that no longer exists -- and it keeps reporting the old answer,
confidently, as often as you like. That is the failure mode this script
exists to make loud:

  * a ctest run reporting "N/N passed" against a stale binary is reporting on
    last week's build, and its test COUNT is the tell: a tree that gained
    tests shows the old, smaller number;
  * an example that "cannot complete playback" may simply have been linked
    before the fix landed.

Two ways a tree goes stale, and the first one this script did NOT catch:
  * a PARTIAL build (`cmake --build --target libX.a`, or a preset narrowed
    with `targets:`) rebuilds a library and leaves every executable that
    links it untouched -- the build reports success. This is the original
    check: binary vs the libraries it links.
  * NO build at all since the sources changed. The whole tree was built on
    Monday, a fix landed on Thursday, and every binary-vs-library comparison
    still passes because the binary and its libraries are all Monday's. That
    gap produced a real incident: play_glfw out of a six-day-old tree
    starved after ~40 frames, which read exactly like a render-path
    regression and was nothing of the kind -- the fix had landed three days
    after the tree was built. The source-vs-binary comparison below closes
    it: a source newer than the executable means the executable embeds code
    that has since been edited.

Exit codes match the other checkers in tools/ (see check_protocols.py):
  0  every comparable executable is at least as new as the libraries it links
     and the sources that feed them
  1  at least one executable is stale (listed on stdout)
  2  nothing could be compared -- no ninja build tree under --root, or the
     tree(s) found carry no readable .ninja_log. A tree we cannot judge
     cannot certify freshness, so this is a failure, not a skip.

HOW STALENESS IS DECIDED
Two comparisons, both anchored on the executable:

  * binary vs the libraries it links, from .ninja_log (not from stat()).
    Ninja records the output mtime it wrote for each edge, and comparing
    log-to-log sidesteps everything the filesystem does between edges
    (CMake's archive rule ends in `cmake -E touch`, so an archive's mtime
    advances on every relink; note the recorded value is
    nanosecond-precision and is not the same number as stat().st_mtime_ns).
    The dependency edges come from build.ninja, so each executable is
    compared only against the libraries it actually links.
  * binary vs the SOURCES that feed it, from stat(). A source's mtime is
    written by editors and git, never by the build system, so stat is the
    authoritative "when did this file last change" -- and if that is newer
    than the executable, the executable predates the change. Sources reach
    the executable two ways: through the objects archived into its linked
    libraries, and through its own object inputs. Objects are never touched
    by a build rule (only archives are), so the object edges are a safe map
    from library to source.

KNOWN LIMITATION, stated rather than papered over: HEADER changes are
invisible here. This generator tracks header dependencies in the binary
.ninja_deps log, not in build.ninja's text edges, and this script
deliberately does not parse binary formats (its whole design is "read the
text logs"). An edit that touches only a .h will therefore pass this gate;
the .cc half of the same edit will not. `ninja -C <tree> -n` was evaluated
as a complete alternative and rejected: on a freshly built tree it answers
"Re-running CMake" forever (the glob re-check cannot settle without a real
build), which would make the gate fire on trees that are not stale.

WHAT COUNTS AS A COMPARISON
An executable built in this tree, against a library from this tree that
ninja also built here, and against the sources ninja compiled into those
libraries and into the executable itself. Both numbers are reported at the
end: a run that compares nothing is exit 2, never a green "everything is
fresh".

--binary NAME narrows the scope to ONE executable (accepts "play_glfw" or
"bin/play_glfw"). That exists for tools that measure exactly one binary:
tools/bench_render.sh runs play_glfw and nothing else, so the honest
question there is whether THAT binary describes the current sources, not
whether some unittest in the same tree does. A tree whose play_glfw is
fresh and whose player_unittests is not must not block a render
measurement. Omitting --binary compares every executable, which is what
the CI gates want: there, a stale anything means the tree is stale.
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
    """Returns (bin/ executable -> the lib/ inputs ninja links it against,
    bin/ executable -> its own object inputs).

    Ninja's edge line is
        build <outs...>: <rule> <explicit ins> | <implicit ins> || <order-only>

    Three things about that grammar are load-bearing here:

    * <outs...> may list SEVERAL outputs on one edge. CMake uses this for the
      test binaries: `build bin/base_unittests
      tests/base_unittests[1]_tests.cmake` links the unittest and emits
      ctest's registration file together. A
      parser that assumes one output per line drops every unittest binary in
      the tree -- silently, because a dropped edge is indistinguishable from a
      fresh one. Every token on the left counts.
    * The libraries live in the IMPLICIT section (after the single `|`), not
      in the explicit inputs, which are the object files.
    * The order-only section (after `||`) repeats library paths purely for
      build ordering. Including it would make every consumer look dependent
      on libraries it merely has to be built after.

    The explicit section is returned as well, not just parsed past: those
    are the executable's own objects, and a source edited behind one of THEM
    is just as invisible to a binary-vs-library comparison as a source
    edited behind a library's objects.
    """
    edges = {}
    own_objects = {}
    with open(build_ninja, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not line.startswith("build bin/"):
                continue
            outputs, colon, rest = line[len("build "):].partition(":")
            if not colon:
                continue
            head = rest.partition("||")[0]
            implicit = head.partition("|")[2]
            explicit = head.partition("|")[0]
            libraries = [
                token for token in implicit.split() if token.startswith("lib/")
            ]
            objects = [
                token for token in explicit.split() if token.endswith(".o")
            ]
            for output in outputs.split():
                if output.startswith("bin/"):
                    edges[output] = libraries
                    own_objects[output] = objects
    return edges, own_objects


def parse_archive_edges(build_ninja):
    """Returns lib/libX.a -> the object files ninja archives into it.

    Objects are the EXPLICIT inputs of the archive edge (before any `|`).
    Only `.o` tokens are kept: some edges carry a response file or a linker
    script alongside them, and neither compiles from a source.
    """
    archives = {}
    with open(build_ninja, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not line.startswith("build lib/"):
                continue
            outputs, colon, rest = line[len("build "):].partition(":")
            if not colon:
                continue
            head = rest.partition("||")[0]
            objects = [token for token in head.split() if token.endswith(".o")]
            for output in outputs.split():
                if output.startswith("lib/"):
                    archives[output] = objects
    return archives


def parse_object_edges(build_ninja):
    """Returns object path -> the source files compiled into it.

    Explicit inputs only, and that is the whole map this checker can build
    from text: headers are tracked in the binary .ninja_deps log by this
    generator, so they do not appear here. See the module docstring's
    KNOWN LIMITATION for what that costs and why `ninja -n` was rejected as
    the way around it.

    Sources are not filtered by extension on purpose: the set includes
    generated inputs (moc output, C shims) that exist on disk after a build
    and are skipped by stat() when they do not.
    """
    objects = {}
    with open(build_ninja, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if not line.startswith("build "):
                continue
            outputs, colon, rest = line[len("build "):].partition(":")
            if not colon:
                continue
            targets = [o for o in outputs.split() if o.endswith(".o")]
            if not targets:
                continue
            head = rest.partition("||")[0]
            # Everything before the first `|` is "<rule> <explicit ins>", so
            # the first token is the rule name and has to come off; keeping it
            # would compare the executable against a string that is not a
            # file. Implicit inputs (after `|`) are kept too: a generated
            # header listed there is a real staleness witness.
            explicit = head.partition("|")[0].split()
            implicit = head.partition("|")[2].split()
            sources = (explicit[1:] if explicit else []) + implicit
            for target in targets:
                objects[target] = sources
    return objects


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


def stat_ns(path):
    """mtime in nanoseconds, or None when the file is not there.

    A source that was generated and has since been removed is skipped rather
    than treated as ancient: there is nothing on disk to compare against,
    and guessing would make the gate fire on a file nobody can inspect.
    """
    try:
        return os.stat(path).st_mtime_ns
    except OSError:
        return None


def inspect_build_dir(build_dir, only=None):
    """Returns (stale descriptions, comparisons made, unusable reason).

    Exactly one of "stale descriptions" and "unusable reason" is meaningful;
    unusable_reason is None for a tree this checker can judge.

    `only` narrows the compared set to one executable (see --binary). When it
    names something the tree does not build, that is exit 2 rather than a
    green run: a binary that cannot be found cannot be certified fresh.
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
        edges, own_objects = parse_bin_edges(ninja)
        archives = parse_archive_edges(ninja)
        object_sources = parse_object_edges(ninja)
    except OSError as exc:
        return None, 0, "%s cannot be read (%s)" % (build_dir, exc)

    if unreadable:
        return None, 0, (
            "%s: %d unreadable row(s) in .ninja_log -- a log that does not "
            "parse cannot certify freshness" % (build_dir, unreadable)
        )
    if not outputs:
        return None, 0, "%s has an empty .ninja_log" % build_dir

    # Source mtimes are stat'ed once per file and shared by every executable
    # that reaches them: a tree has hundreds of objects but only a few
    # thousand distinct sources, and the walk below visits each object once
    # per executable that links it.
    source_mtimes = {}

    def newest_source(libraries, own):
        """(mtime_ns, path) of the newest source feeding these objects.

        The objects of the linked libraries, plus the executable's own
        objects, mapped to their sources through build.ninja's object edges.
        Missing files (generated and since removed) are skipped.
        """
        objects = list(own)
        for library in libraries:
            objects.extend(archives.get(library, []))
        best = None
        for obj in objects:
            for src in object_sources.get(obj, []):
                if os.path.isabs(src):
                    path = src
                else:
                    path = os.path.join(build_dir, src)
                if path not in source_mtimes:
                    source_mtimes[path] = stat_ns(path)
                mtime = source_mtimes[path]
                if mtime is not None and (best is None or mtime > best[0]):
                    best = (mtime, path)
        return best

    stale = []
    compared = 0
    candidates = sorted(set(edges) | set(own_objects))
    if only is not None:
        wanted = only if "/" in only else "bin/" + only
        candidates = [binary for binary in candidates if binary == wanted]
        if not candidates:
            return None, 0, (
                "%s builds no executable named %s -- a binary that cannot be "
                "found cannot be certified fresh" % (build_dir, only)
            )
    for binary in candidates:
        binary_mtime = outputs.get(binary)
        if binary_mtime is None:
            continue  # ninja has never built it here; nothing to compare
        libraries = edges.get(binary, [])
        linked = [
            (outputs[path], path) for path in libraries if path in outputs
        ]
        source = newest_source(libraries, own_objects.get(binary, []))
        if not linked and source is None:
            continue  # links nothing this tree built, and owns no objects
        compared += 1

        # Comparison 1: the library the executable was linked against
        # changed after the link (a partial build).
        if linked:
            newest_input, newest_input_path = max(linked)
            if binary_mtime < newest_input:
                stale.append(
                    "  %s is older than %s (the library changed after the last "
                    "link; anything you measure with this binary describes old "
                    "code)" % (binary, newest_input_path)
                )
                continue

        # Comparison 2: a SOURCE that feeds this executable changed after it
        # was linked (no build at all since the edit). The executable's own
        # mtime is used here, not the log's: nothing in the build system
        # writes an executable, so stat is the honest "when was this linked".
        binary_stat = stat_ns(os.path.join(build_dir, binary))
        if (binary_stat is not None and source is not None
                and binary_stat < source[0]):
            stale.append(
                "  %s is older than %s (that source changed after this binary "
                "was linked; the fix you are about to measure is not in this "
                "binary)" % (binary, source[1])
            )
    return stale, compared, None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        default=".",
        help="directory to search for build trees (default: the current one)",
    )
    parser.add_argument(
        "--binary",
        default=None,
        help="compare only this executable (\"play_glfw\" or "
        "\"bin/play_glfw\"); every executable is compared when omitted",
    )
    args = parser.parse_args()

    build_dirs = find_build_dirs(args.root)
    if not build_dirs:
        print(
        "check_binary_freshness: no ninja build tree under %s" % args.root
    )
        return 2

    compared = 0
    stale_trees = 0
    total_stale = 0
    unusable = []
    for build_dir in build_dirs:
        stale, count, reason = inspect_build_dir(build_dir, args.binary)
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
                "  fix: the tree does not describe the current sources. "
                "Rebuild the targets you are about to measure -- "
                "`cmake --build <dir>` (or `--target <name>` for one binary) "
                "-- and run this again."
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
        "tree(s), each at least as new as the libraries it links and the "
        "sources that feed them"
        % (compared, len(build_dirs) - len(unusable))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
