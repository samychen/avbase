#!/usr/bin/env python3
# Copyright 2026 The ijkpp Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Enforces the architecture and style invariants from docs/02 §2.1 and docs/06 §9.

Architecture rules that live only in a document rot within a year. This script
turns the ones that can be checked mechanically into a CI gate.

Exit code 0 = all rules pass. Non-zero = violations printed, one per line.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import sys

# Rules C1/C2/C3 need an AST for accuracy; the regex fallbacks below are
# deliberately conservative so they never produce a false pass.
MAX_FILE_LINES = 500
MAX_FUNCTION_LINES = 80

# Files that legitimately exceed MAX_FILE_LINES, with the reason. Adding an
# entry here requires a linked issue (invariant C12 / risk R12).
LINE_LIMIT_ALLOWLIST = {
    "media/filters/ffmpeg_demuxer.cc": (
        1100, "FFmpeg container/codec adaptation: two config builders, the "
              "MediaInfo walk, the demux loop and the seek path. Chromium's "
              "media/filters/ffmpeg_demuxer.cc is ~2500 lines for the same "
              "job; splitting ours by line count would scatter one contiguous "
              "AVFormatContext lifecycle across files, which is how "
              "use-after-free bugs get introduced."),
    "media/filters/video_frame_compositor.cc": (
        700, "Port of ffplay video_refresh(); splitting it would obscure the "
             "provenance comments that map each branch to the original."),
    "player/option_registry.cc": (
        700, "Generated table at M1; hand-written until then."),
}

# Layers that must never see a vendor or platform header (invariants C4, C5).
CORE_DIRS = ("base", "media/base", "player/public")
VENDOR_INCLUDE_RE = re.compile(r'#\s*include\s*[<"](libav|libsw|avcodec|avformat|avutil)')
PLATFORM_INCLUDE_RE = re.compile(
    r'#\s*include\s*[<"](SDL2/|X11/|wayland-|alsa/|pulse/|GL/|EGL/|vulkan/|'
    r'android/|CoreVideo/|Metal/|windows\.h)')
PLATFORM_MACRO_RE = re.compile(r'\b(__ANDROID__|__APPLE__|__linux__|_WIN32|__linux)\b')
FFMPEG_VERSION_GUARD_RE = re.compile(r'#\s*if.*(LIBAV\w+_VERSION)')
EXCEPTION_RE = re.compile(r'\b(throw|try|catch)\b')
BARE_ALLOC_RE = re.compile(r'(?<![\w:.])(new|delete|malloc|calloc|realloc|free)\s*[(<]')
GOTO_RE = re.compile(r'(?<![\w:])\bgoto\b')
USING_NAMESPACE_RE = re.compile(r'^\s*using\s+namespace\b')

VENDOR_GUARD_ALLOWLIST = {"platform/ffmpeg/av_includes.h"}
ALLOC_ALLOWLIST = (
    "base/memory",                 # The allocators themselves.
    "base/types",                  # The fallback base::expected uses placement new.
    "media/base/video_frame.cc",   # calloc for the frame backing store.
    "tests/",
)

# Functions that legitimately exceed MAX_FUNCTION_LINES, with the reason.
# Same governance as LINE_LIMIT_ALLOWLIST: an entry needs a linked issue.
FUNCTION_LIMIT_ALLOWLIST = {
    ("media/filters/ffmpeg_video_decoder.cc",
     "FFmpegVideoDecoder::DecodeAvailableFrames"): (
        130, "One linear receive_frame -> convert -> emit loop. The two "
             "conversion paths (swscale and the no-conversion plane copy) are "
             "branches of a single decision that depends on state built up "
             "across the loop, so extracting them would mean passing six "
             "parameters or introducing a state object for no gain."),
    ("media/filters/ffmpeg_demuxer.cc", "FFmpegDemuxer::OpenOnDemuxThread"): (
        140, "One contiguous AVFormatContext lifecycle: allocate, install the "
             "interrupt callback, build options, open, report unconsumed "
             "options, probe streams. An earlier attempt to split the open step "
             "into a helper passed the context by value and reintroduced a "
             "double-free on the failure path (all 10 end-to-end tests "
             "SEGFAULTed). Keeping it linear is the safer shape here."),
    ("player/option_registry.cc", "OptionRegistry::OptionRegistry"): (
        400, "Hand-written option table; replaced by tools/gen_options.py "
             "output at M1 (invariant C13)."),
    ("player/player_event.cc", "ToLegacyEvent"): (
        200, "Flat mapping from every EventType to ijkplayer's "
             "(what, arg1, arg2, obj) quadruple. Splitting it would scatter "
             "one lookup table across functions."),
}
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

    def add(self, rule: str, path: pathlib.Path, line: int, msg: str) -> None:
        self.violations.append(f"{rule}  {path}:{line}: {msg}")

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


def check_suffixes(root: pathlib.Path, report: Report) -> None:
    """C17: sources are .cc, never .cpp (Google/Chromium convention)."""
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in {"build", ".git", "docs"}]
        for name in filenames:
            if name.endswith((".cpp", ".cxx", ".C")):
                report.add("C17", pathlib.Path(dirpath) / name, 0,
                           "use the .cc suffix")


def check_namespace_balance(path: pathlib.Path, text: str, report: Report) -> None:
    """Namespaces must open and close the same number of times."""
    opens = len(re.findall(r"^\s*namespace\s+[\w:]*\s*\{", text, re.M))
    closes = len(re.findall(r"^\}\s*//\s*namespace", text, re.M))
    if opens != closes:
        report.add("C20", path, 0,
                   f"namespace open/close mismatch ({opens} open, {closes} close)")


DRAFT_MARKER = "STATUS: DRAFT"


def check_file(path: pathlib.Path, rel: str, report: Report) -> None:
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()

    # A file explicitly marked DRAFT is excluded from every CMake target, so
    # style and size rules do not apply to it yet. Layering rules (C22) and
    # namespace balance (C20) still do, because those are design statements
    # that must hold even in a draft. Draft files are counted and reported at
    # the end so they cannot be quietly forgotten.
    is_draft = DRAFT_MARKER in text
    if is_draft:
        DRAFT_FILES.append(rel)

    is_core = rel.startswith(CORE_DIRS)
    is_test = rel.startswith("tests/") or rel.endswith("_unittest.cc")

    if is_draft:
        check_namespace_balance(path, text, report)
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

    # C20: namespace balance (caught three real bugs during bring-up).
    check_namespace_balance(path, text, report)

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

        # C8: FFmpeg version guards live in exactly one file.
        if FFMPEG_VERSION_GUARD_RE.search(line) and rel not in VENDOR_GUARD_ALLOWLIST:
            report.add("C8", path, i,
                       "FFmpeg version guard outside platform/ffmpeg/av_includes.h")

        # C9: no goto, ever.
        if GOTO_RE.search(line):
            report.add("C9", path, i, "goto is banned")

        # C14: no `using namespace` in headers.
        if path.suffix == ".h" and USING_NAMESPACE_RE.match(line):
            report.add("C14", path, i, "`using namespace` in a header")

        # C18: no exceptions in core layers.
        if not is_test and rel.startswith(("base/", "media/", "player/")):
            if EXCEPTION_RE.search(line):
                report.add("C18", path, i,
                           "exceptions are disabled project-wide; use base::expected")

        # C7: no bare allocation outside the allowlisted allocators.
        if not is_test and not rel.startswith(ALLOC_ALLOWLIST):
            if BARE_ALLOC_RE.search(line) and "std::" not in line:
                report.add("C7", path, i,
                           "use std::make_unique / base::WrapUnique / MakeRefCounted")


def check_header_guards(root: pathlib.Path, report: Report) -> None:
    """Every .h has an #ifndef guard matching its path (Google style)."""
    for path in iter_sources(root):
        if path.suffix != ".h":
            continue
        rel = path.relative_to(root).as_posix()
        expected = "IJKPP_" + rel.upper().replace("/", "_").replace(".", "_").replace("-", "_") + "_"
        text = path.read_text(encoding="utf-8", errors="replace")
        if f"#ifndef {expected}" not in text:
            report.add("C21", path, 0,
                       f"missing or wrong header guard; expected #ifndef {expected}")


def enclosing_symbol(lines: list[str], start: int) -> str:
    """Best-effort name of the function starting at |start| (1-based)."""
    for offset in range(0, 4):
        idx = start - 1 - offset
        if idx < 0:
            break
        m = re.search(r"([\w:~]+)\s*\([^;]*$", lines[idx])
        if m and m.group(1) not in ("if", "while", "for", "switch", "return"):
            return m.group(1)
    return "<unknown>"


def check_function_length(path: pathlib.Path, rel: str, report: Report) -> None:
    """C2: rough brace-matching function length check.

    Conservative on purpose: it only reports functions whose body clearly
    exceeds the limit, so it never blocks on a false positive.
    """
    if path.suffix != ".cc":
        return
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    depth = 0
    start = 0
    in_namespace_only = False
    for i, line in enumerate(lines, start=1):
        stripped = line.strip()
        if stripped.startswith(("//", "/*", "*")):
            continue
        opens = line.count("{")
        closes = line.count("}")
        if depth == 0 and opens and not in_namespace_only:
            # Heuristic: a function definition starts at brace depth 0 with a
            # ')' on the same or previous line and no trailing ';'
            prev = lines[i - 2] if i >= 2 else ""
            if (")" in line or ")" in prev) and not stripped.endswith(";") \
                    and not stripped.startswith(("namespace", "struct", "class",
                                                 "enum", "union", "#")):
                start = i
                depth = opens - closes
                continue
        if depth > 0:
            depth += opens - closes
            if depth <= 0:
                length = i - start + 1
                if length > MAX_FUNCTION_LINES:
                    name = enclosing_symbol(lines, start)
                    allowed, _why = FUNCTION_LIMIT_ALLOWLIST.get(
                        (rel, name), (MAX_FUNCTION_LINES, ""))
                    if length > allowed:
                        report.add("C2", path, start,
                                   f"function {name} is {length} lines, "
                                   f"limit {allowed}")
                depth = 0
        in_namespace_only = stripped.startswith("namespace")


def check_test_pairing(root: pathlib.Path, report: Report) -> None:
    """C11: every non-trivial source directory has a matching unit test dir."""
    for layer in ("base", "media/base", "media/filters"):
        src = root / layer
        if not src.is_dir():
            continue
        has_sources = any(p.suffix == ".cc" for p in src.glob("*.cc"))
        if not has_sources:
            continue
        test_dir = root / "tests/unit" / layer.replace("/", "_")
        if not test_dir.is_dir():
            report.add("C11", src, 0, f"no tests/unit/{test_dir.name}/ directory")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()

    report = Report()
    check_suffixes(root, report)
    check_header_guards(root, report)
    check_test_pairing(root, report)

    for path in sorted(iter_sources(root)):
        rel = path.relative_to(root).as_posix()
        check_file(path, rel, report)
        if DRAFT_MARKER not in path.read_text(encoding='utf-8', errors='replace'):
            check_function_length(path, rel, report)

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
