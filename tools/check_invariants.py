#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Enforces the structural invariants from docs/02 §2.1 and docs/06 §9.

Slimmed to the seven rules whose violations hide real defects -- the ones the
build system or the compiler cannot catch on its own:

  C1   file length (the 500-line ratchet that forces real seams to be named)
  C4   FFmpeg headers stay in their quarantine (no-ffmpeg build survives)
  C5   platform headers/macros stay out of the core layers
  C22  media/ and base/ never depend on player/
  C23  80-column ratchet
  C24  every source a target lists exists
  C25  every .cc in the tree is compiled by some target

The ten style rules this gate used to carry (C2, C7-C9, C11, C14, C17, C18,
C20, C21) were cut in the "keep only what finds bugs" pass: C18 (throw) is
already a compile error under -fno-exceptions and C20 (namespace balance)
under any compiler, and the rest encode taste, not structure. IDs are not
reassigned so history stays greppable.

Exit code 0 = all rules pass. Non-zero = violations printed, one per line.
"""

from __future__ import annotations

import argparse
import glob
import os
import pathlib
import re
import sys

# Rules C1/C2/C3 need an AST for accuracy; the regex fallbacks below are
# deliberately conservative so they never produce a false pass.
MAX_FILE_LINES = 500
MAX_COLUMNS = 80
# C23's ratchet baseline: "<path> <count of lines over MAX_COLUMNS>" per line.
COLUMN_BASELINE = "tools/column_baseline.txt"

# Files that legitimately exceed MAX_FILE_LINES, with the reason. Adding an
# entry here requires a linked issue (invariant C12 / risk R12).
LINE_LIMIT_ALLOWLIST = {
    "media/filters/ffmpeg_demuxer.cc": (
        1100, "FFmpeg container/codec adaptation: two config builders, the "
              "MediaInfo walk, the demux loop and the seek path. Chromium's "
              "media/filters/ffmpeg_demuxer.cc is ~2500 lines for the same "
              "job; splitting ours by line count would scatter one contiguous "
              "AVFormatContext lifecycle across files, which is how "
              "use-after-free bugs get introduced. Track selection and the "
              "text leg were split out to ffmpeg_demuxer_track_select.cc "
              "(1137 -> 1080), so this ceiling is now ratcheted: it may only "
              "move down."),
    "media/filters/legacy/video_frame_compositor.cc": (
        700, "Port of ffplay video_refresh(); splitting it would obscure the "
             "provenance comments that map each branch to the original. "
             "LGPL-2.1 quarantine, see media/filters/legacy/README.md."),
    "player/option_registry.cc": (
        700, "Generated table at M1; hand-written until then."),
    "media/filters/audio_renderer_algorithm.cc": (
        660, "WSOLA buffering, index bookkeeping and the three FillBuffer "
             "modes. It was 756 lines before wsola_internals and "
             "audio_frame_queue were split out, which is the split Chromium "
             "uses too; the remaining overshoot is the queue-sizing block "
             "(SetLatencyHint, IsQueueAdequateForPlayback, IsQueueFull, "
             "IncreasePlaybackThreshold), which overlaps M9 BufferController's "
             "three-tier high water mark. Whether that policy belongs here or "
             "there is a design question for M9, and answering it by cutting a "
             "file to a line count would be the wrong reason. Raised "
             "620 -> 660 when SearchBlockFrames() and "
             "EffectiveSearchBlockFrames() were added to shrink the search "
             "region at end of stream; that is index bookkeeping, which is "
             "this file's stated reason to exist, and the measured "
             "before/after numbers live in the comment on CanPerformWsola(). "
             "See the LENGTH note in the file header."),
    "media/filters/renderer_impl.cc": (
        600, "Sub-renderer lifecycle plumbing: initialize/flush/ended fan-out "
             "across the video and audio halves plus the buffering-verdict "
             "wiring from M9's starvation work. The two halves mirror each "
             "other call for call; splitting video from audio would duplicate "
             "the state machine instead of naming a seam. Revisit when M9 "
             "BufferController absorbs CheckBufferingTransitions()."),
}

# Layers that must never see a vendor or platform header (invariants C4, C5).
CORE_DIRS = ("base", "media/base", "player/public")
VENDOR_INCLUDE_RE = re.compile(r'#\s*include\s*[<"](libav|libsw|avcodec|avformat|avutil)')
PLATFORM_INCLUDE_RE = re.compile(
    r'#\s*include\s*[<"](SDL2/|X11/|wayland-|alsa/|pulse/|GL/|EGL/|vulkan/|'
    r'android/|CoreVideo/|Metal/|windows\.h)')
PLATFORM_MACRO_RE = re.compile(r'\b(__ANDROID__|__APPLE__|__linux__|_WIN32|__linux)\b')

# Header comments legitimately mention platform names when documenting rules.
# Chromium splits platform-specific base/ code into per-platform translation
# units selected by the build system (platform_thread_posix.cc,
# platform_thread_win.cc, ...). Those files ARE base/'s platform abstraction
# layer, so the C5 "no platform macros in core" rule exempts them by suffix.
# The exemption is narrow: it applies only under base/ and only to files whose
# name ends in a recognised platform suffix.
PLATFORM_SUFFIX_RE = re.compile(
    r"_(posix|linux|win|mac|ios|android|fuchsia|epoll|x11|wayland)\.(cc|h)$")


DRAFT_FILES: list[str] = []


class Report:
    def __init__(self) -> None:
        self.violations: list[str] = []
        # Non-fatal observations. Kept separate from violations so that "the
        # baseline has a stale entry" cannot be mistaken for "the code is wrong",
        # and so that notes never affect the exit code.
        self.notes: list[str] = []

    def add(self, rule: str, path: pathlib.Path, line: int, msg: str) -> None:
        self.violations.append(f"{rule}  {path}:{line}: {msg}")

    def note(self, msg: str) -> None:
        self.notes.append(msg)

    def ok(self) -> bool:
        return not self.violations


def iter_sources(root: pathlib.Path):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames
                       if d not in {"build", ".git", "third_party", "docs"}
                       and not d.startswith("build")]
        for name in filenames:
            if name.endswith((".h", ".cc")):
                yield pathlib.Path(dirpath) / name


DRAFT_MARKER = "STATUS: DRAFT"


def check_file(path: pathlib.Path, rel: str, report: Report) -> None:
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    # A file explicitly marked DRAFT is excluded from every CMake target, so
    # the size and layering rules do not apply to it yet. Layering (C22) still
    # does, because it is a design statement that must hold even in a draft.
    # Draft files are counted and reported at the end so they cannot be
    # quietly forgotten.
    is_draft = DRAFT_MARKER in text
    if is_draft:
        DRAFT_FILES.append(rel)

    is_core = rel.startswith(CORE_DIRS)
    is_test = rel.startswith("tests/") or rel.endswith("_unittest.cc")

    if is_draft:
        for i, raw_line in enumerate(lines, start=1):
            if rel.startswith(("base/", "media/")) and \
                    re.search(r'#\s*include\s*"player/', raw_line):
                report.add("C22", path, i, "media/ or base/ includes player/")
        return

    # C1: file length. Test files are exempt, matching Chromium: a suite grows
    # with the behaviour it covers and splitting it by size hurts discoverability.
    if is_test:
        limit = 4000
    else:
        limit, _reason = LINE_LIMIT_ALLOWLIST.get(rel, (MAX_FILE_LINES, ""))
    if len(lines) > limit:
        report.add("C1", path, len(lines),
                   f"file is {len(lines)} lines, limit {limit}")

    for i, raw_line in enumerate(lines, start=1):
        stripped = raw_line.strip()
        if stripped.startswith("//") or stripped.startswith("*"):
            continue
        # Keyword rules must look at CODE, not at prose. Two false-positive
        # sources, both hit in practice:
        #   * trailing comments  — "catch up", "retry", "throw away"
        #   * string literals    — user-facing error text such as
        #     "verify the URI is reachable (try `ffprobe ...`)"
        # Strip both before matching, otherwise C18 fires on documentation.
        line = re.sub(r'"(?:\\.|[^"\\])*"', '""', raw_line)
        line = re.sub(r"'(?:\\.|[^'\\])*'", "''", line)
        line = re.sub(r"//.*$", "", line)
        if not line.strip():
            continue

        # C4: no FFmpeg headers outside platform/ffmpeg and media/filters/ffmpeg_*.
        if VENDOR_INCLUDE_RE.search(line):
            if not (rel.startswith("platform/ffmpeg/") or
                    "ffmpeg" in pathlib.Path(rel).name):
                report.add("C4", path, i, "FFmpeg header in a core file")

        # C5: no platform headers or macros in base/ media/ player/.
        # Exempt: base/'s per-platform translation units (see PLATFORM_SUFFIX_RE).
        is_platform_tu = rel.startswith("base/") and bool(
            PLATFORM_SUFFIX_RE.search(rel))
        if (rel.startswith(("base/", "media/", "player/")) and not is_test
                and not is_platform_tu):
            if PLATFORM_INCLUDE_RE.search(line):
                report.add("C5", path, i, "platform header in a core file")
            if PLATFORM_MACRO_RE.search(line) and not stripped.startswith("*"):
                report.add("C5", path, i, "platform macro in a core file")

        # C22: media/ and base/ must never depend on player/. The dependency
        # direction is player -> media -> base (docs/02 §2.1); a reverse include
        # makes the media layer unbuildable without the SDK facade. Types that
        # both layers need live in media/base and are re-exported by
        # player/public (MediaError, MediaInfo, NativeDisplay,
        # DataSourceDescriptor all follow this pattern).
        if rel.startswith(("base/", "media/")) and not is_test:
            if re.search(r'#\s*include\s*"player/', line):
                report.add("C22", path, i,
                           "media/ or base/ includes player/ — invert the "
                           "dependency: move the type to media/base and "
                           "re-export it from player/public")

def load_column_baseline(root: pathlib.Path) -> dict:
    path = root / COLUMN_BASELINE
    if not path.exists():
        return {}
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.rsplit(" ", 1)
        if len(parts) == 2 and parts[1].isdigit():
            out[parts[0]] = int(parts[1])
    return out


# Target names may contain hyphens (avbase-inspect), and a source path may not
# contain whitespace -- the first version of this rule allowed spaces in the
# path character class, so it captured the indentation along with the filename
# and reported every source in the tree as missing. A rule that reports
# everything is worse than no rule: it gets deleted, and then the real breakage
# it was written for goes uncaught.


CMAKE_CALL_RE = re.compile(r"\b(?:add_library|add_executable)\s*\(")
CMAKE_SOURCE_RE = re.compile(r"(?<![\w./-])([\w./-]+\.(?:cc|cpp|c))(?![\w./-])")


def cmake_target_calls(text: str):
    r"""Yields (target, body) for every add_library/add_executable in |text|.

    The body is found by counting parentheses rather than by looking for a blank
    line. The blank-line version was wrong in a way that mattered: comments are
    stripped before matching, which turns a comment-only line into a
    whitespace-only line, and a whitespace-only line satisfies the same
    "\n\s*\n" that a blank line does -- so the body was truncated at the first
    comment block and every source listed after it went unchecked. That is how
    this rule passed while a genuinely missing source file sat in the list.
    """
    for m in CMAKE_CALL_RE.finditer(text):
        depth = 0
        i = m.end() - 1          # at the '('
        start = m.end()
        while i < len(text):
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = text[start:i]
        head = body.split("\n", 1)[0]
        target = head.strip().split()[0] if head.strip() else "?"
        yield target, body


def check_cmake_sources(root: pathlib.Path, report: Report) -> None:
    """C24: every source file named in add_library/add_executable must exist.

    Added because a bad edit to tests/CMakeLists.txt once left a fragment of a
    deleted comment in the SOURCES list -- a bare `It` with no `#` in front of
    it -- and CMake failed with "Cannot find source file: It". Fifteen rules ran
    green over that tree, because none of them looked inside a CMakeLists.

    Comments are stripped before matching, which is the discipline C18 needed:
    a source path mentioned in prose is not a source file. Without stripping,
    media/CMakeLists.txt reports a missing media/base/demuxer.cc purely because
    a comment refers to it.
    """
    for cm in sorted(root.rglob("CMakeLists.txt")):
        if any(part in (".git", "build", "third_party") for part in cm.parts):
            continue
        text = re.sub(r"#[^\n]*", "",
                      cm.read_text(encoding="utf-8", errors="replace"))
        for target, body in cmake_target_calls(text):
            if re.search(r"\b(IMPORTED|ALIAS|INTERFACE)\b", body.split("\n")[0]):
                continue          # no source list to validate
            for src in CMAKE_SOURCE_RE.findall(body):
                if "$" in src or "{" in src:
                    continue      # generated or variable-driven path
                if not (cm.parent / src).exists():
                    report.add("C24", cm, 0,
                               f"target {target} lists {src}, which does not "
                               f"exist relative to "
                               f"{cm.parent.relative_to(root) or '.'}")


CMAKE_GLOB_RE = re.compile(
    r"file\s*\(\s*GLOB(?:_RECURSE)?\s+([A-Za-z_]\w*)(.*?)\)", re.S)
CMAKE_GLOB_PATTERN_RE = re.compile(r"\"([^\"]+)\"")


def globbed_sources(cm: pathlib.Path, text: str) -> list[pathlib.Path]:
    """Expands the file(GLOB ...) calls of one CMakeLists into real paths.

    A static checker cannot ask CMake to expand a pattern, so it does it itself:
    relative patterns resolve against the CMakeLists' own directory, and
    ${CMAKE_CURRENT_SOURCE_DIR} is substituted (CMake would have done that
    before globbing). Only quoted patterns count -- an unquoted argument is a
    variable name, and expanding it as a literal path would silently cover
    nothing, which is how a rule ends up green and useless.
    """
    out: list[pathlib.Path] = []
    for _var, args in CMAKE_GLOB_RE.findall(text):
        for raw in CMAKE_GLOB_PATTERN_RE.findall(args):
            pat = raw.replace("${CMAKE_CURRENT_SOURCE_DIR}",
                              cm.parent.as_posix())
            path = pathlib.Path(pat)
            if not path.is_absolute():
                path = cm.parent / path
            out.extend(pathlib.Path(hit).resolve()
                       for hit in glob.glob(str(path)))
    return out


def check_sources_in_a_target(root: pathlib.Path, report: Report) -> None:
    """C25: every .cc in the tree is compiled by some target.

    C24 answers "a listed file exists". The other direction was silent: a .cc
    that no list mentions is simply never compiled, and a test suite running
    green over a tree that is missing a component is worse than a red one. The
    rule became reachable when part of the lists turned into file(GLOB ...)
    (docs/06 §7.6), so those globs are resolved here and the rule means the same
    thing on both sides of the tree.

    A file that must not be compiled yet is named in DRAFT_FILES. Note what a
    glob changes about that workflow: a DRAFT .cc left in a globbed directory is
    compiled whether or not it is exempted here. Globs do not recurse, so the
    place for it is a subdirectory no target reaches until it builds.
    """
    covered: set[pathlib.Path] = set()
    for cm in sorted(root.rglob("CMakeLists.txt")):
        if any(part in (".git", "build", "third_party") for part in cm.parts):
            continue
        text = re.sub(r"#[^\n]*", "",
                      cm.read_text(encoding="utf-8", errors="replace"))
        covered.update(globbed_sources(cm, text))
        # Whole-file, not "inside add_library": the test suites pass their
        # sources through the avbase_add_unittest() helper, so the only
        # add_executable in tests/CMakeLists.txt is the one inside the function
        # body with ${T_SOURCES} -- a target-body-only scan reported all seven
        # files listed there as uncompiled. What the rule needs to know is
        # "some build file names it", and that is what this asks.
        for src in CMAKE_SOURCE_RE.findall(text):
            if "$" in src or "{" in src:
                continue          # variable-driven path, covered by its glob
            covered.add((cm.parent / src).resolve())

    for path in sorted(root.rglob("*.cc")):
        if any(part in (".git", "build", "third_party")
               or part.startswith("build") for part in path.parts):
            continue
        rel = path.relative_to(root).as_posix()
        if path.resolve() in covered or rel in DRAFT_FILES:
            continue
        report.add("C25", path, 0,
                   "no target compiles this file: name it in a CMakeLists, or "
                   "list it in DRAFT_FILES if it must not be built yet "
                   "(docs/06 §7.6)")


def check_columns(root: pathlib.Path, report: Report, update: bool) -> None:
    """C23: column width, as a ratchet rather than a hard limit.

    .clang-format declares ColumnLimit 80 and STYLE.md calls it mandatory, but
    nothing enforced it: clang-tidy does not check width, clang-format is not run
    in CI, and no check-format job exists. Counting the existing codebase found
    323 lines over 80 columns across 80 files -- the tree has never been
    formatted to its own stated limit.

    Adding a hard rule would turn CI red on 323 pre-existing lines and teach
    everyone to ignore it. Reformatting them all in one pass is worse: there is
    no clang-format binary and no compiler here, so the diff would be
    unverifiable, and rewrapping template declarations by hand can change
    meaning. So the rule ratchets: a file may not gain over-length lines, and a
    new file may not have any. The baseline only ever shrinks, which is the point
    -- every file touched for another reason comes back clean and stays clean.

    DRAFT files are exempt, consistent with every other style rule here: they are
    not in a build, and their authors are already working without a compiler.
    """
    baseline = load_column_baseline(root)
    current: dict = {}
    for path in sorted(iter_sources(root)):
        rel = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        if DRAFT_MARKER in text:
            continue
        over = [(i, len(line)) for i, line in enumerate(text.splitlines(), 1)
                if len(line) > MAX_COLUMNS]
        if over:
            current[rel] = len(over)
    if update:
        write_column_baseline(root, baseline, current, report)
        return
    for rel, count in sorted(current.items()):
        allowed = baseline.get(rel, 0)
        if count > allowed:
            report.add("C23", root / rel, count,
                       f"{count} line(s) over {MAX_COLUMNS} columns, baseline "
                       f"allows {allowed}; rewrap them (clang-format -i {rel})")
    for rel in sorted(set(baseline) - set(current)):
        # Not a failure: the file was cleaned up, or moved, or became DRAFT.
        # Reported so the baseline gets regenerated instead of quietly rotting.
        report.note(f"C23 baseline entry is stale: {rel} "
                    f"({baseline[rel]} line(s)) -- run --update-baseline")
    total_now = sum(current.values())
    total_base = sum(baseline.values())
    if total_now < total_base:
        print(f"C23: {total_now} over-length line(s), down from {total_base} "
              f"in the baseline")
    elif total_now == total_base:
        print(f"C23: {total_now} over-length line(s), unchanged from baseline")


def write_column_baseline(root: pathlib.Path, old: dict, current: dict,
                          report: Report) -> None:
    # Bootstrapping is not the same as raising an allowance. With no baseline
    # file yet, every entry looks like growth from zero, and refusing to write
    # would make the rule impossible to adopt -- which is how a gate that should
    # exist ends up never existing. So: no baseline file on disk means record the
    # current state and say loudly that this is a starting point, not an
    # approval. Once the file exists, growth is refused.
    bootstrapping = not (root / COLUMN_BASELINE).exists()
    grew = {} if bootstrapping else {
        r: (old.get(r, 0), c) for r, c in current.items()
        if c > old.get(r, 0)}
    if grew:
        # Growing the baseline is how a ratchet dies, so it is never silent.
        for rel, (before, after) in sorted(grew.items()):
            report.add("C23", root / rel, after,
                       f"--update-baseline would RAISE the allowance from "
                       f"{before} to {after}; fix the lines instead")
        return
    lines = [
        "# C23 column-width ratchet baseline. Regenerate with:",
        "#   python3 tools/check_invariants.py --root . --update-baseline",
        "#",
        "# Each entry is '<path> <number of lines over 80 columns>'. The tool",
        "# refuses to raise an entry, so this file can only shrink. Do not edit",
        "# it by hand: an entry added by hand is an exemption with no reason",
        "# attached, which is what R12 says must not happen.",
        f"# Total: {sum(current.values())} line(s) in {len(current)} file(s).",
        "",
    ]
    lines += [f"{rel} {count}" for rel, count in sorted(current.items())]
    (root / COLUMN_BASELINE).write_text("\n".join(lines) + "\n")
    total = sum(current.values())
    if bootstrapping:
        print(f"C23: baseline CREATED with {total} over-length line(s) in "
              f"{len(current)} file(s). This records the existing debt so the "
              f"rule can start gating; it is not an endorsement of it. Every "
              f"file touched from now on must not add to its count, and "
              f"--update-baseline refuses to raise any entry.")
    else:
        shrunk = sum(old.values()) - total
        print(f"C23: baseline written -- {total} over-length line(s) in "
              f"{len(current)} file(s)"
              + (f", {shrunk} fewer than before" if shrunk > 0 else ""))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--update-baseline", action="store_true",
                        help="regenerate the C23 column-width baseline; refuses "
                             "to raise any entry")
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()

    report = Report()

    for path in sorted(iter_sources(root)):
        rel = path.relative_to(root).as_posix()
        check_file(path, rel, report)

    check_cmake_sources(root, report)
    check_sources_in_a_target(root, report)
    check_columns(root, report, args.update_baseline)

    for note in report.notes:
        print(f"note: {note}")
    if DRAFT_FILES:
        print(f"note: {len(DRAFT_FILES)} DRAFT file(s) excluded from style/size "
              f"rules (they are not in any build target):")
        for d in DRAFT_FILES:
            print(f"        {d}")

    if report.ok():
        print("check_invariants: all rules pass "
              f"({sum(1 for _ in iter_sources(root))} files scanned)")
        return 0

    print(f"check_invariants: {len(report.violations)} violation(s)\n", file=sys.stderr)
    for v in report.violations:
        print("  " + v, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
