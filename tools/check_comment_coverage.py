#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""V9 (docs/10 §5 and §12): the public headers must be self-explanatory.

docs/10 §5 requires every public method's comment to answer four questions:

    (1) what it does            -- one line
    (2) which thread/sequence, and whether it blocks
    (3) when it fails and what it returns
    (4) side effects / which events it emits

§12 turns that into a checkable item (V9): comment coverage of
player/public/*.h public methods >= 95%, with the four dimensions present.
This script is that check. It parses the headers with a line state machine
(no libclang dependency: CI runners would need it installed, and the public
surface is written in a deliberately flat style the state machine handles),
then reports, per declaration:

    exists  -- has a leading block or a trailing note at all
    do/thread/block/error/side -- which of the four questions the comment
              answers, by keyword signal (see the RE_* patterns below)
    full    -- all four answered

Class-level comments inherit their signals to the class's methods (the
Player class states its thread-safety contract once at class scope, exactly
like Chromium media does; requiring every method to repeat it would train
people to write noise).

Scope: public sections of classes/structs in player/public/**/*.h, plus the
file-level C ABI functions in player/public/c/avbase_c.h. Two exclusions
match docs/Doxyfile's (same surface, same rule): avbase_c_internal.h and
avbase_c_export.h are implementation-side, and special members
(constructors, destructors, operator overloads) are not counted -- their
behaviour is fixed by the language and the class contract. Data-only structs
(payloads, configs) have no methods and contribute nothing; their fields are
not methods and are not counted.

The signals are deliberately keyword-based, not semantic: they catch "this
comment says nothing about blocking", they do not prove the comment is true.
That is the honest scope of a comment gate.

Usage
-----
    tools/check_comment_coverage.py                 # gate: existence >= 95%
    tools/check_comment_coverage.py --report        # per-declaration detail
    tools/check_comment_coverage.py --min-existence-rate 95 --min-full-rate 0
                                                     # the CI gate command
    tools/check_comment_coverage.py --json          # machine readable
    tools/check_comment_coverage.py --selftest      # parser + signal checks
                                                     # on synthetic input

Exit codes: 0 = thresholds met, 1 = below threshold, 2 = usage/parse error.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# --- The four questions, as keyword signals ---------------------------------
# "Thread safety: may be called from any thread" / "runs on the media
# sequence" both answer (2)'s thread half; "non-blocking"/"returns
# immediately"/"blocks until" answer its blocking half.
RE_THREAD = re.compile(r"\b(thread|sequence)\b", re.IGNORECASE)
RE_BLOCK = re.compile(r"\bblock", re.IGNORECASE)  # blocking/blocked/non-blocking
# "Errors: kInvalidState when ..." / "fails if ...". The k-prefix alternative
# is restricted to error-code spellings so that a getter documented as
# "returns kIdle" is not credited with an error section it does not have.
RE_ERROR = re.compile(
    r"\berrors?\b|\bfail|\binvalid|"
    r"\bk(?:Invalid|No|OutOf|Unsupported|Failed|Error|Not)\w*",
    re.IGNORECASE)
# "Side effects: emits kSeekCompleted" / "triggers a reset" / "modifies state".
RE_SIDE = re.compile(r"side.effects?|emits?|triggers?|modif|fires?",
                     re.IGNORECASE)
# The labelled paragraphs §5's example uses. Stripped before deciding whether
# the comment answers (1): a comment consisting only of "Errors: none" says
# nothing about what the method DOES.
LABELLED_RE = re.compile(
    r"^\s*(thread safety|threading|errors?|side effects?|note|watch out)\s*:",
    re.IGNORECASE)

ACCESS_RE = re.compile(r"^\s*(public|private|protected)\s*:")
TYPE_KEYWORDS = ("class", "struct", "enum", "union", "namespace", "typedef",
                 "using", "friend", "return", "static_assert", "template")
# Statement-start tokens: a line beginning with one of these is never a
# declaration prefix, so the fallback below still clears the pending comment
# for it (and never tries to start a multi-line signature from it).
STMT_KEYWORDS = ("if", "for", "while", "switch", "throw", "delete", "new",
                 "goto", "do", "else", "case", "break", "continue")
# Call-like tokens that are statements or casts, not method names.
CALL_NAME_BLOCKLIST = {"if", "for", "while", "switch", "return", "sizeof",
                       "void", "const", "static_cast", "reinterpret_cast"}


@dataclass
class Decl:
    kind: str            # "method" | "function"
    name: str
    location: str        # file:line
    class_ctx: str       # "" for file-level functions
    leading: list[str] = field(default_factory=list)
    trailing: str = ""
    class_leading: list[str] = field(default_factory=list)
    file_leading: list[str] = field(default_factory=list)

    def comment_text(self) -> str:
        return "\n".join(self.leading + ([self.trailing] if self.trailing
                                         else []))

    def signals(self) -> dict[str, bool]:
        """Which of the four questions this declaration's comment answers.

        The method's own comment first; the class-level comment (or, for the
        C ABI's file-level functions, the header's contract block) can supply
        thread/block/error/side (inherited), but never (1) -- a class or file
        blurb does not say what each function does.
        """
        own = self.comment_text()
        inherited = "\n".join(self.class_leading + self.file_leading)
        prose = "\n".join(line for line in own.splitlines()
                          if not LABELLED_RE.match(line))
        return {
            "do": bool(own.strip()) and bool(prose.strip()),
            "thread": bool(RE_THREAD.search(own) or RE_THREAD.search(inherited)),
            "block": bool(RE_BLOCK.search(own) or RE_BLOCK.search(inherited)),
            "error": bool(RE_ERROR.search(own) or RE_ERROR.search(inherited)),
            "side": bool(RE_SIDE.search(own) or RE_SIDE.search(inherited)),
        }


@dataclass
class Scope:
    kind: str                       # "class" | "namespace" | "block"
    name: str
    public: bool                    # class/struct access section in effect
    comment: list[str]              # leading block attached to the scope


def strip_comment_marker(line: str) -> str:
    text = line.strip()
    if text.startswith("//"):
        text = text[2:]
    if text.startswith("///"):
        text = text[3:]
    return text.strip()


def find_top_level_call(line: str) -> str | None:
    """Name of the first call whose `(` is outside templates/braces/strings.

    `Result<std::unique_ptr<Player>> Build();`      -> "Build"
    `std::function<void()> cb;`                     -> None (void( is inside
                                                          the template args)
    `std::optional<Seconds> timeout{Seconds(30)};`  -> None (inside braces)
    `void (*cb)(int);`                              -> None (`(` follows `*`)
    """
    angle = brace = 0
    quote = ""
    i = 0
    n = len(line)
    while i < n:
        ch = line[i]
        if quote:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
        elif ch == "<":
            angle += 1
        elif ch == ">":
            angle = max(0, angle - 1)
        elif ch == "{":
            brace += 1
        elif ch == "}":
            brace = max(0, brace - 1)
        elif ch == "(" and angle == 0 and brace == 0:
            j = i - 1
            while j >= 0 and line[j].isspace():
                j -= 1
            end = j + 1
            while j >= 0 and (line[j].isalnum() or line[j] in "_~"):
                j -= 1
            name = line[j + 1:end]
            if not name or name in CALL_NAME_BLOCKLIST:
                return None
            # An ALL-CAPS callee is a macro invocation
            # (AVBASE_DECLARE_EVENT_ACCESSOR(AsPrepared, PreparedPayload)),
            # not a function declaration.
            if re.fullmatch(r"[A-Z][A-Z0-9_]*", name):
                return None
            return name
        i += 1
    return None


def parse_header(text: str, path: str) -> list[Decl]:
    """Collect public methods (and file-level functions) from one header."""
    decls: list[Decl] = []
    pending: list[str] = []       # comment block that may attach to a decl
    pending_gap = False           # a blank line separates it from the decl
    file_pending: list[str] = []  # everything before the first `#` line
    file_comment: list[str] = []  # header contract block (first one only)
    scopes: list[Scope] = []
    pending_scope: Scope | None = None  # class/namespace whose `{` is later
    sig_lines: list[str] = []     # accumulated multi-line signature
    sig_start = 0
    in_define_cont = False        # inside a backslash-continued #define

    def class_path() -> str:
        return "::".join(s.name for s in scopes if s.kind == "class")

    def in_public() -> bool:
        for scope in reversed(scopes):
            if scope.kind == "class":
                return scope.public
        return False

    def is_method() -> bool:
        return any(s.kind == "class" for s in scopes)

    def flush_signature() -> None:
        nonlocal sig_lines
        if not sig_lines:
            return
        sig = " ".join(sig_lines).strip()
        sig_lines = []
        name = find_top_level_call(sig)
        if not name:
            return
        leading = [] if pending_gap else list(pending)
        if is_method():
            if not in_public():
                return
            ctx = class_path()
            comment = [s.comment for s in scopes if s.kind == "class"][-1]
            # Special members are excluded: their behaviour is fixed by the
            # language and the class contract (destruction is bounded by
            # PlayerConfig::shutdown_timeout, copying is deleted), so a
            # four-question comment on them would be noise. Ordinary
            # constructors that DO carry preconditions stay in -- they are
            # spelled out in the class-level comment.
            short_ctx = ctx.rsplit("::", 1)[-1]
            if (name == short_ctx or name == "~" + short_ctx
                    or name.startswith("operator")):
                return
        else:
            ctx = ""
            comment = []
        decls.append(Decl(kind="method" if is_method() else "function",
                          name=name, location=f"{path}:{sig_start}",
                          class_ctx=ctx, leading=leading, trailing="",
                          class_leading=list(comment),
                          file_leading=list(file_comment)))

    def sig_complete() -> bool:
        # A declaration is complete without a trailing `;` when it carries an
        # inline body: `Type Name(...) { ... }`. Inline definitions in headers
        # (empty `{}` or a one-liner) never terminate with `;`, so the scanner
        # must flush on the closing `}` instead -- otherwise every inline
        # method after the first in a class is swallowed into one blob and
        # dropped (V9 would undercount real coverage).
        s = " ".join(sig_lines)
        depth = 0
        for ch in s:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
        return depth <= 0 and "}" in s

    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.rstrip()
        stripped = line.strip()

        if sig_lines:
            # Inside a multi-line signature: accumulate until the semicolon.
            trailing = ""
            if "//" in line:
                code, _, comment = line.partition("//")
                sig_lines.append(code)
                trailing = comment.strip()
            else:
                sig_lines.append(line)
            if ";" in line:
                flush_signature()
                if trailing and decls:
                    decls[-1].trailing = trailing
                pending.clear()
                pending_gap = False
            elif sig_complete():
                # Inline definition (`}`) terminates the declaration without a
                # semicolon; flush it here so every inline method is counted
                # and credited with its own comment.
                flush_signature()
                pending.clear()
                pending_gap = False
            continue

        if not stripped:
            # A blank line ends the current comment block: what follows is a
            # new block that must re-attach on its own.
            pending_gap = True
            continue

        if stripped.startswith("//"):
            body = strip_comment_marker(stripped)
            # Section banners ("// ---- Lifecycle ----") are navigation, not
            # documentation. Counting them would credit the next method with
            # whatever keywords the banner happens to carry ("thread-safe,
            # non-blocking" in the Queries banner), which is exactly the
            # false-positive shape a comment gate must not have. Same rule as
            # tools/doxygen_comment_filter.py applies to banners.
            if re.match(r"^[-=*_#]{3,}", body):
                continue
            if pending_gap:
                pending = []
                pending_gap = False
            pending.append(body)
            if not file_comment:
                file_pending.append(body)
            continue

        if stripped.startswith("#"):
            # The block before the first preprocessor line is the header's
            # contract (avbase_c.h states its threading and blocking rules
            # there); file-level functions inherit from it the way class
            # methods inherit from their class comment.
            if not file_comment and file_pending:
                file_comment = list(file_pending)
            pending.clear()
            pending_gap = False
            in_define_cont = stripped.endswith("\\")
            continue

        if in_define_cont:
            # Continuation line of a multi-line #define (e.g. the
            # AVBASE_DECLARE_EVENT_ACCESSOR body): not a declaration.
            in_define_cont = stripped.endswith("\\")
            continue

        if ACCESS_RE.match(stripped):
            for scope in reversed(scopes):
                if scope.kind == "class":
                    scope.public = stripped.startswith("public")
                    break
            pending.clear()
            continue

        # class/struct/namespace heads open a scope. The brace may share the
        # line or arrive later (inheritance clause on its own line), so a
        # head without `{` parks in pending_scope until the brace shows up.
        head = re.match(r"(?:template\s*<[^>]*>\s*)?"
                        r"(class|struct|namespace)\s+"
                        r"(?:[A-Z][A-Z0-9_]*\s+)*"
                        r"([A-Za-z_][A-Za-z0-9_]*)?", stripped)
        if head and not stripped.endswith(";"):
            kind = head.group(1)
            name = head.group(2) or ""
            scope = Scope(kind="class" if kind in ("class", "struct") else
                          "namespace", name=name,
                          public=(kind == "struct"),
                          comment=list(pending))
            pending.clear()
            if "{" in stripped:
                scopes.append(scope)
            else:
                pending_scope = scope
            continue

        first = stripped.split()[0] if stripped.split() else ""
        if first in TYPE_KEYWORDS:
            pending.clear()
            continue

        # Brace bookkeeping. A scope parked in pending_scope (its `{` arrived
        # on a later line, e.g. after an inheritance clause) consumes this
        # line's opening brace, so it must not also count as a nested block.
        opens = stripped.count("{")
        closes = stripped.count("}")
        if pending_scope is not None and opens > closes:
            scopes.append(pending_scope)
            pending_scope = None
            opens -= 1
        for _ in range(opens - closes):
            scopes.append(Scope(kind="block", name="", public=False,
                                comment=[]))
        for _ in range(closes - opens):
            if scopes:
                scopes.pop()

        # C ABI functions carry an AVBASE_C_API marker before the return
        # type; strip it so the declaration scan sees a plain signature.
        scan = stripped
        if first.endswith("_API"):
            scan = stripped[len(first):].strip()

        if ";" in scan:
            code, sep, comment = scan.partition("//")
            if find_top_level_call(code):
                sig_lines = [code.rstrip()]
                sig_start = lineno
                flush_signature()
                if decls and sep:
                    decls[-1].trailing = comment.strip()
            pending.clear()
            pending_gap = False
            continue

        if find_top_level_call(scan):
            sig_lines = [scan]
            sig_start = lineno
            # A single-line inline definition (`Type Name(...) {}`) has no `;`;
            # flush it now so it becomes its own declaration.
            if sig_complete():
                flush_signature()
                pending.clear()
                pending_gap = False
            continue

        if first.endswith("_API"):
            # Return type on its own line ("AVBASE_C_API AvbaseStatus" then
            # the name on the next): the signature starts here, so the
            # pending comment must survive until the name line completes it.
            sig_lines = [scan]
            sig_start = lineno
            continue

        # A line with no `;`, no `{`/`}`, and no call yet is a declaration
        # prefix that continues onto the next line (e.g.
        # `AVBASE_PLAYER_EXPORT std::vector<ConfigIssue>` sitting above the
        # function name). Accumulate it as the head of a multi-line signature
        # and KEEP the pending comment, so the comment above a split return
        # type is not lost (V9 would otherwise undercount real coverage).
        # Statement-like lines still clear the pending comment as before.
        if (";" not in stripped and "{" not in stripped
                and "}" not in stripped and first not in STMT_KEYWORDS
                and not find_top_level_call(scan)):
            sig_lines = [scan]
            sig_start = lineno
            continue
        pending.clear()

    return decls


# Same exclusions as docs/Doxyfile: these two are implementation-side files,
# not the public surface.
EXCLUDED_NAMES = {"avbase_c_internal.h", "avbase_c_export.h"}


def collect(root: Path) -> list[Decl]:
    decls: list[Decl] = []
    for path in sorted(root.rglob("*.h")):
        if path.name in EXCLUDED_NAMES:
            continue
        text = path.read_text(encoding="utf-8")
        decls.extend(parse_header(text, str(path.relative_to(root.parent))))
    return decls


def summarize(decls: list[Decl]) -> dict:
    total = len(decls)
    exists = sum(1 for d in decls if d.comment_text().strip())
    full = 0
    dim_totals = {"do": 0, "thread": 0, "block": 0, "error": 0, "side": 0}
    for d in decls:
        sig = d.signals()
        for key, hit in sig.items():
            dim_totals[key] += 1 if hit else 0
        if all(sig.values()):
            full += 1
    return {
        "total": total,
        "exists": exists,
        "full": full,
        "existence_rate": (exists / total) if total else 1.0,
        "full_rate": (full / total) if total else 1.0,
        "dimensions": {k: (v / total if total else 1.0)
                       for k, v in dim_totals.items()},
    }


# --- Selftest -----------------------------------------------------------------
# Synthetic input covering the shapes the state machine must get right; run
# with --selftest so CI can prove the parser works without the real headers.

SELFTEST_INPUT = """\
// File header: every function below may be called from any thread.
#ifndef GUARD_H_
#define GUARD_H_

namespace avbase {

// A widget does widget things.
//
// Thread safety: all methods may be called from any thread.
class AVBASE_EXPORT Widget {
 public:
  // Starts the widget.
  void Start();

  // ---- Navigation banner: not documentation ----
  void AfterBanner();

  void Pause();  // Idempotent.

  Status Stop(int timeout);

  // Resets everything.
  //
  // Errors: kInvalidState when playing.
  void Reset();

  // A function-typed field is not a method.
  std::function<void()> on_done;

  // Neither is a brace-initialised field.
  std::optional<Seconds> timeout{Seconds(30)};

 private:
  void Hidden();

 protected:
  bool AlsoHidden() const;
};

// A class with no access section: private by default.
class Plain {
  // Not collected.
  void ImplicitlyPrivate();

 public:
  // Explicitly public.
  void Public();
};

// A free function.
AVBASE_C_API void c_free_thing(int* out);

// Inherits on a later line.
class Derived
    : public Widget {
 public:
  void DerivedMethod();
};

enum class Color {
  kRed,
  kGreen,
};

// A macro invocation is not a function declaration.
DECLARE_THING(AsFoo, FooPayload);

// A multi-line define's body is not a declaration either.
#define DECLARE_THING(Name, Type) \
  const Type* Name(const Event& event)

// An empty struct: no methods, no braces to balance.
struct Empty {};

}  // namespace avbase
#endif
"""


def selftest() -> int:
    decls = parse_header(SELFTEST_INPUT, "selftest.h")
    got = [(d.class_ctx, d.name) for d in decls]
    want = [
        ("Widget", "Start"),
        ("Widget", "AfterBanner"),
        ("Widget", "Pause"),
        ("Widget", "Stop"),
        ("Widget", "Reset"),
        ("Plain", "Public"),
        ("", "c_free_thing"),
        ("Derived", "DerivedMethod"),
    ]
    assert got == want, f"parse mismatch:\n got: {got}\nwant: {want}"

    by_name = {d.name: d for d in decls}
    # A navigation banner must not become the next method's comment.
    assert by_name["AfterBanner"].comment_text() == "", \
        "section banner counted as documentation"
    # Non-method shapes must not leak in.
    for absent in ("Hidden", "AlsoHidden", "ImplicitlyPrivate", "on_done",
                   "timeout", "Name", "DECLARE_THING"):
        assert absent not in by_name, f"{absent} leaked into the public set"
    # Class-level thread safety is inherited by every public method.
    for name in ("Start", "Pause", "Stop", "Reset"):
        assert by_name[name].signals()["thread"], f"{name}: no thread signal"
    # Trailing notes count as comments; leading blocks carry their text.
    assert by_name["Pause"].comment_text() == "Idempotent."
    assert by_name["Start"].comment_text() == "Starts the widget."
    assert by_name["Stop"].comment_text() == ""
    # Reset: do + thread(inherited) + error, and "Resets everything" alone
    # must NOT count as a side-effect statement.
    sig = by_name["Reset"].signals()
    assert sig["do"] and sig["error"] and not sig["side"], sig
    # A comment made only of labelled paragraphs answers nothing about (1).
    labelled = Decl(kind="method", name="x", location="t:1", class_ctx="C",
                    leading=["Errors: none."], class_leading=[])
    assert labelled.signals()["do"] is False, "labelled-only counts as prose"
    # A getter documented with a state enumerant is not credited with errors.
    getter = Decl(kind="method", name="state", location="t:1", class_ctx="C",
                  leading=["Returns kIdle when constructed."],
                  class_leading=[])
    assert getter.signals()["error"] is False, "kIdle counted as an error code"
    # The C ABI function is collected at file scope.
    assert by_name["c_free_thing"].kind == "function"
    assert by_name["c_free_thing"].class_ctx == ""
    # The header contract block is inherited by file-level functions.
    assert by_name["c_free_thing"].signals()["thread"], \
        "file-level contract not inherited"
    print("selftest: parser and signal checks passed")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default="player/public",
                        help="header tree to check (default: player/public)")
    parser.add_argument("--min-existence-rate", type=float, default=95.0,
                        help="minimum %% of declarations with any comment")
    parser.add_argument("--min-full-rate", type=float, default=0.0,
                        help="minimum %% of declarations answering all four "
                             "questions (0 = report only)")
    parser.add_argument("--report", action="store_true",
                        help="list every declaration that is missing "
                             "something")
    parser.add_argument("--json", action="store_true",
                        help="machine-readable summary")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    root = Path(args.root)
    if not root.is_dir():
        print(f"no such directory: {root}", file=sys.stderr)
        return 2
    decls = collect(root)
    if not decls:
        print(f"no declarations found under {root}", file=sys.stderr)
        return 2

    summary = summarize(decls)
    exit_code = 0
    if summary["existence_rate"] * 100 < args.min_existence_rate - 1e-9:
        exit_code = 1
    if summary["full_rate"] * 100 < args.min_full_rate - 1e-9:
        exit_code = 1

    if args.json:
        print(json.dumps({"summary": summary,
                          "thresholds": {
                              "min_existence_rate": args.min_existence_rate,
                              "min_full_rate": args.min_full_rate}},
                         indent=2))
        return exit_code

    dim = summary["dimensions"]
    print(f"V9 comment coverage ({root}): {summary['total']} public "
          f"declarations")
    print(f"  exists : {summary['exists']:3d}/{summary['total']} "
          f"({summary['existence_rate']*100:.1f}%, gate "
          f">= {args.min_existence_rate:.0f}%)")
    print(f"  full 4 : {summary['full']:3d}/{summary['total']} "
          f"({summary['full_rate']*100:.1f}%, gate >= {args.min_full_rate:.0f}%)")
    print("  dimensions: " + "  ".join(
        f"{k} {v*100:.0f}%" for k, v in dim.items()))

    if args.report:
        print("\nper-declaration detail:")
        for d in decls:
            sig = d.signals()
            where = f"{d.class_ctx}::{d.name}" if d.class_ctx else d.name
            if not d.comment_text().strip():
                status = "NO COMMENT"
            elif all(sig.values()):
                continue
            else:
                missing = [k for k, v in sig.items() if not v]
                status = "missing " + ",".join(missing)
            print(f"  {d.location:50s} {where:60s} {status}")

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
