#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Doxygen INPUT_FILTER: make the repo's plain-// doc comments visible.

avbase's public headers document with plain `//` blocks that PRECEDE the
declaration they describe, plus trailing `// "option-key"` notes. Doxygen
ignores both forms (only ///, //!, /** */ count). Rewriting the headers to
doxygen syntax would be churn on a frozen surface, so this filter rewrites a
copy as doxygen reads it:

  leading block   -- `// text` lines immediately before a declaration line
                     (anything that is not `#`, `}`, or another comment)
                     become `/// text`.
  trailing note   -- `code  // note` becomes `code  ///< note`, doxygen's
                     member post-comment, so field-level notes such as
                     `size_t min_frames{5};  // "min-frames"` survive.

What is deliberately NOT rewritten:
  * comment blocks before a preprocessor line that is not `#define`
    (license headers, include guards, `#include` groups);
  * comment blocks before a closing brace;
  * pure separator blocks (`// ---- Lifecycle ----`): attached to the next
    member they would become its brief, which is worse than dropping them;
  * `//` inside string and character literals -- the scanner below tracks
    quote state, so `"http://x"` is never mistaken for a comment.

Usage:
    tools/doxygen_comment_filter.py <file>   # filtered copy to stdout
    tools/doxygen_comment_filter.py          # same, reading stdin

Doxygen (1.18, verified with an argv probe) calls this as
`<filter> <abs-input-path>`; the stdin form is kept as a fallback for other
configurations. A filter that yields empty output makes doxygen parse an
empty file SILENTLY -- zero warnings, all documentation gone -- which is why
tools/gen_api_docs.sh asserts the XML actually carries prose afterwards.
"""

from __future__ import annotations

import re
import sys

SEPARATOR_RE = re.compile(r"^[\s\-=*_#<>|~/]+$")
# Section banners in these headers open with three or more dashes/equals
# (`// ---- Lifecycle ----`). Attached to the next member they would become
# its brief, so they are left as plain // and simply do not reach doxygen.
BANNER_RE = re.compile(r"^\s*//\s*[-=*_#]{3,}")


def split_code_and_comment(line: str) -> tuple[str, str | None]:
    """Split |line| at the first `//` that is outside a string/char literal.

    Returns (code, comment) where comment includes the `//` and is None when
    the line carries no comment.
    """
    quote: str | None = None
    i = 0
    n = len(line)
    while i < n:
        ch = line[i]
        if quote is not None:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = None
        elif ch in "\"'":
            quote = ch
        elif ch == "/" and i + 1 < n and line[i + 1] == "/":
            return line[:i], line[i:]
        i += 1
    return line, None


def is_comment_line(line: str) -> bool:
    code, comment = split_code_and_comment(line)
    return comment is not None and comment.startswith("//") and not code.strip()


def convert_leading_block(block: list[str]) -> list[str]:
    if BANNER_RE.match(block[0]):
        return list(block)
    # Indented comment lines are code samples in these headers (see the
    # "Minimal use" block in player.h). Doxygen reflows an unwrapped block
    # into a single prose line, so each run of indented lines is wrapped in
    # \code / \endcode, which doxygen renders verbatim. "Indented" means
    # indented PAST the canonical space that follows `//`.
    out: list[str] = []
    in_code = False
    for line in block:
        code, comment = split_code_and_comment(line)
        raw = comment[2:]
        ending = "\n" if raw.endswith("\n") else ""
        text = raw[:-1] if ending else raw
        body = text[1:] if text.startswith(" ") else text
        stripped = body.strip()
        is_code = (body[:1] in (" ", "\t") and bool(stripped)
                   and not SEPARATOR_RE.match(stripped))
        if is_code and not in_code:
            out.append(code + "/// \\code" + ending)
            in_code = True
        elif not is_code and in_code:
            out.append(code + "/// \\endcode" + ending)
            in_code = False
        if is_code or not SEPARATOR_RE.match(stripped):
            out.append(code + "///" + text + ending)
        else:
            out.append(line)
    if in_code:
        out.append("/// \\endcode\n")
    return out


def filter_text(text: str) -> str:
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if is_comment_line(line):
            # Collect the maximal run of comment lines.
            j = i
            while j < n and is_comment_line(lines[j]):
                j += 1
            # What follows the block? Skip blank lines to find out.
            k = j
            while k < n and not lines[k].strip():
                k += 1
            follow = lines[k].strip() if k < n else ""
            # A block before a declaration documents it. `#define` counts as
            # a declaration (the C ABI documents its constant groups that
            # way); the other preprocessor forms do not -- the license header
            # and the include guard must stay plain //.
            convertible = bool(follow) and not follow.startswith(
                ("}", "//", "/*")) and not (
                follow.startswith("#") and not follow.startswith("#define"))
            if convertible:
                out.extend(convert_leading_block(lines[i:j]))
            else:
                out.extend(lines[i:j])
            i = j
            continue
        code, comment = split_code_and_comment(line)
        stripped_code = code.strip()
        if (comment is not None and stripped_code
                and not stripped_code.startswith("#")
                and not stripped_code.endswith("}")):
            # Trailing note after code: doxygen member post-comment.
            text_after = comment[2:]
            if not SEPARATOR_RE.match(text_after.strip()):
                out.append(code.rstrip() + " ///<" + text_after)
            else:
                out.append(line)
        else:
            out.append(line)
        i += 1
    return "".join(out)


def main() -> int:
    if len(sys.argv) > 2:
        print("usage: doxygen_comment_filter.py [file]", file=sys.stderr)
        return 2
    if len(sys.argv) == 2:
        # The doxygen INPUT_FILTER form: the input file is passed as argv[1]
        # (verified against doxygen 1.18 via an argv probe).
        with open(sys.argv[1], encoding="utf-8") as f:
            data = f.read()
    else:
        # Defensive: some doxygen versions/configurations feed stdin instead.
        data = sys.stdin.read()
    sys.stdout.write(filter_text(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
