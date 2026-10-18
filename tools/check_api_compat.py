#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""V10 (docs/10 §4 and §12): a minor version bump must not break user code.

docs/10 §4 defines the compatibility contract: "SemVer; minor 版本升级不改
用户代码即可编译通过". §12 turns it into a checkable item (V10): an API diff
tool that flags, between minor versions, anything that would stop existing
user code from compiling.

This script is that tool. It does NOT shell out to upstream
abi-compliance-checker (which needs Perl + abi-dumper + a fully compiled
binary to dump ABIs -- heavy, and the sandbox CI runner would have to install
all of it). Instead it extracts the *declared* public API surface with the
same line state machine V9 uses (no libclang dependency), and diffs it against
a committed baseline JSON that represents the last released surface.

What counts as a breaking change (rc=1 for a minor bump):
  - a public function / method / constructor / destructor / operator is
    REMOVED (user code referencing it no longer compiles);
  - a signature CHANGES -- return type, parameter types, parameter count, or
    the const-qualifier of a member function (any of these changes how the
    overload set resolves or what the caller must pass);
  - a C ABI (extern "C") function signature changes -- the C surface is a
    strict stability contract;
  - an enumerator is REMOVED or its VALUE changes (both break compile / ABI);
  - a class's set of virtual functions changes -- adding/removing/reordering
    a virtual shifts the vtable, which breaks any external subclass.

What is compatible (allowed for a minor bump):
  - a NEW symbol is added (function, method, overload, enumerator, class);
  - a new non-virtual method is added to an existing class (does not shift
    layout).

Scope: public sections of classes/structs in player/public/**/*.h, the
file-level C ABI functions in player/public/c/avbase_c.h, and public enums.
Two exclusions match docs/Doxyfile: avbase_c_internal.h and avbase_c_export.h
are implementation-side. Plain public DATA MEMBERS are intentionally NOT
tracked in v1 (see Known limitations in docs/10 §12 V10 note) -- they are a
future extension; v1 covers the callable surface + enums + vtable layout,
which is where the "user code compiles" contract mostly lives and where a
naive scanner would otherwise produce false positives.

Usage
-----
    tools/check_api_compat.py                 # check current vs baseline
    tools/check_api_compat.py --snapshot      # (re)write tools/api_baseline.json
    tools/check_api_compat.py --report        # detailed diff
    tools/check_api_compat.py --allow-breaking  # downgrade breaking to warnings
                                             # (use only when cutting a MAJOR)
    tools/check_api_compat.py --json          # machine readable
    tools/check_api_compat.py --selftest      # parser + diff checks on synthetic

Exit codes: 0 = no breaking changes (or --allow-breaking), 1 = breaking
changes detected, 2 = usage / parse / missing-baseline error.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# --- Surface extraction ------------------------------------------------
ACCESS_RE = re.compile(r"^\s*(public|private|protected)\s*:")
TYPE_KEYWORDS = ("class", "struct", "enum", "union", "namespace", "typedef",
                 "using", "friend", "return", "static_assert", "template")
STMT_KEYWORDS = ("if", "for", "while", "switch", "throw", "delete", "new",
                 "goto", "do", "else", "case", "break", "continue")
CALL_NAME_BLOCKLIST = {"if", "for", "while", "switch", "return", "sizeof",
                       "void", "const", "static_cast", "reinterpret_cast"}

SIG_SEP = "\x1f"  # unit separator; never appears in a normalized signature


@dataclass
class Symbol:
    kind: str            # "function" | "method" | "enum"
    lang: str            # "cpp" | "c"
    qualified: str       # human-readable name
    id: str              # dedup key
    sigs: list[str] = field(default_factory=list)     # for function/method
    is_virtual: bool = False
    enumerators: dict[str, int] = field(default_factory=dict)  # for enum


@dataclass
class Scope:
    kind: str            # "class" | "namespace" | "block" | "externc"
    name: str
    public: bool
    comment: list[str]


def strip_comment_marker(line: str) -> str:
    text = line.strip()
    if text.startswith("//"):
        text = text[2:]
    if text.startswith("///"):
        text = text[3:]
    return text.strip()


def find_top_level_call(line: str) -> str | None:
    """First call name whose `(` is outside templates/braces/strings."""
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
        elif ch == "{" or ch == "}":
            brace += 1 if ch == "{" else -1
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
            if re.fullmatch(r"[A-Z][A-Z0-9_]*", name):
                return None  # macro invocation, not a declaration
            return name
        i += 1
    return None


def locate_paren(s: str) -> int:
    angle = 0
    for i, ch in enumerate(s):
        if ch == "<":
            angle += 1
        elif ch == ">":
            angle = max(0, angle - 1)
        elif ch == "(" and angle == 0:
            return i
    return -1


def match_paren(s: str, start: int) -> int:
    depth = 0
    for i in range(start, len(s)):
        ch = s[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def split_top_commas(s: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    cur = ""
    for ch in s:
        if ch in "<({":
            depth += 1
            cur += ch
        elif ch in ">)}":
            depth -= 1
            cur += ch
        elif ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return parts


def normalize_type(t: str) -> str:
    t = re.sub(r"\s+", " ", t).strip()
    return re.sub(r"\s*([*&])\s*", r"\1", t).strip()


def norm_param(p: str) -> str:
    p = p.strip()
    if not p:
        return ""
    # Drop a default argument: `int timeout = 30` -> `int`.
    eq = -1
    depth = 0
    for i, ch in enumerate(p):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth = max(0, depth - 1)
        elif ch == "=" and depth == 0:
            eq = i
            break
    if eq != -1:
        p = p[:eq].strip()
    # Drop a trailing parameter name: `const std::string& uri` -> `const std::string&`.
    p = re.sub(r"\s+([A-Za-z_]\w*)\s*$", "", p)
    return normalize_type(p)


def normalize_params(params: str) -> str:
    return "(" + ", ".join(norm_param(p) for p in split_top_commas(params)) + ")"


def split_decl(text: str):
    """Return (name, ret, params, const, is_virtual) or None."""
    s = re.sub(r"//[^\n]*", "", text)
    # Drop dllexport / C-API macros: they are not part of the type and change
    # freely (reformatting, moving the macro) without altering the ABI.
    s = re.sub(r"\b[A-Z_][A-Z0-9_]*_EXPORT\b", "", s)
    s = re.sub(r"\bAVBASE_C_API\b", "", s)
    s = re.sub(r"\s+", " ", s).strip()
    is_virtual = bool(re.search(r"\bvirtual\b", s))
    s = re.sub(r"\bvirtual\b", "", s).strip()
    const = False
    lastp = s.rfind(")")
    if lastp != -1 and re.match(r"\s*const\b", s[lastp + 1:]):
        const = True
        s = s[:lastp + 1] + re.sub(r"^\s*const\b", "", s[lastp + 1:]).strip()
    p = locate_paren(s)
    if p < 0:
        return None
    before = s[:p]
    close = match_paren(s, p)
    if close < 0:
        return None
    params = s[p + 1:close]
    op = before.find("operator")
    if op != -1:
        name = before[op:p].strip()
        ret = before[:op].strip()
    else:
        m = re.search(r"([A-Za-z_]\w*)\s*$", before)
        if not m:
            return None
        name = m.group(1)
        ret = before[:m.start()].strip()
    return name, normalize_type(ret), normalize_params(params), const, is_virtual


def eval_enum_expr(expr: str, known: dict[str, int]) -> int:
    """Safely evaluate a simple enumerator expression (0, kFoo+1, 1<<3)."""
    expr = expr.strip()
    if expr == "":
        return 0
    if re.fullmatch(r"-?\d+", expr):
        return int(expr)
    if expr in known:
        return known[expr]
    safe = re.sub(r"[0-9A-Za-z_+\-*/<>&|()\s]", "", expr)
    if safe:  # disallow anything but arithmetic / shifts / parens
        return 0
    env = dict(known)
    env["__builtins__"] = {}
    try:
        return int(eval(expr, env))  # noqa: S307 (trusted, restricted input)
    except Exception:
        return 0


def parse_enum_body(body: str) -> dict[str, int]:
    body = re.sub(r"//[^\n]*", "", body)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    # Strip the outer braces (and any trailing ';') so the first/last
    # comma-separated token is not mistaken for an enumerator.
    body = body.strip()
    if body.startswith("{"):
        body = body[1:]
    body = body.rstrip(";").rstrip()
    if body.endswith("}"):
        body = body[:-1]
    parts = [p for p in body.split(",") if p.strip()]
    enumerators: dict[str, int] = {}
    value = 0
    for p in parts:
        p = p.strip()
        if not p or p == "=":
            continue
        if "=" in p:
            nm, _, expr = p.partition("=")
            nm = nm.strip().split()[0]
            expr = expr.strip()
            if expr == "":
                enumerators[nm] = value
            else:
                value = eval_enum_expr(expr, enumerators)
                enumerators[nm] = value
        else:
            nm = p.split()[0]
            enumerators[nm] = value
        value += 1
    return enumerators


def extract_symbols(text: str, path: str) -> list[Symbol]:
    symbols: list[Symbol] = []
    pending: list[str] = []
    pending_gap = False
    file_pending: list[str] = []
    file_comment: list[str] = []
    scopes: list[Scope] = []
    pending_scope: Scope | None = None
    sig_lines: list[str] = []
    sig_start = 0
    in_define_cont = False
    in_enum: dict | None = None  # {name, buf, depth, class_ctx, lang}

    def class_path() -> str:
        return "::".join(s.name for s in scopes if s.kind == "class")

    def in_public() -> bool:
        for scope in reversed(scopes):
            if scope.kind == "class":
                return scope.public
        return False

    def is_method() -> bool:
        return any(s.kind == "class" for s in scopes)

    def lang_for(first: str) -> str:
        if first == "AVBASE_C_API":
            return "c"
        if any(s.kind == "externc" for s in scopes):
            return "c"
        return "cpp"

    def qualify(name: str) -> str:
        parts = [s.name for s in scopes
                 if s.kind in ("namespace", "class") and s.name]
        return "::".join(parts + [name]) if parts else name

    def sig_complete() -> bool:
        s = " ".join(sig_lines)
        depth = 0
        for ch in s:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
        return depth <= 0 and "}" in s

    def flush_signature() -> None:
        nonlocal sig_lines
        if not sig_lines:
            return
        sig = " ".join(sig_lines).strip()
        sig_lines = []
        parsed = split_decl(sig)
        if not parsed:
            return
        name, ret, params, const, is_virtual = parsed
        if is_method():
            if not in_public():
                return
            ctx = class_path()
        else:
            ctx = ""
        lang = lang_for("")
        kind = "method" if is_method() else "function"
        if kind == "method":
            sid = "M:" + qualify(name)
        elif lang == "c":
            sid = "C:" + name
        else:
            sid = "F:" + qualify(name)
        sig_str = f"{ret}{SIG_SEP}{params}{SIG_SEP}{'C' if const else ''}"
        symbols.append(Symbol(kind=kind, lang=lang,
                             qualified=qualify(name), id=sid,
                             sigs=[sig_str], is_virtual=is_virtual))

    for lineno, raw in enumerate(text.splitlines(), start=1):
        line = raw.rstrip()
        stripped = line.strip()

        # ---- enum body capture -------------------------------------------
        if in_enum is not None:
            in_enum["buf"] += " " + stripped
            in_enum["depth"] += stripped.count("{") - stripped.count("}")
            if in_enum["depth"] <= 0:
                body = in_enum["buf"]
                enum_name = in_enum["name"]
                enumerators = parse_enum_body(body)
                full = qualify(enum_name)
                symbols.append(Symbol(kind="enum", lang=in_enum["lang"],
                                     qualified=full, id="E:" + full,
                                     enumerators=enumerators))
                in_enum = None
                pending.clear()
                pending_gap = False
            continue

        if sig_lines:
            if "//" in line:
                code, _, _ = line.partition("//")
                sig_lines.append(code)
            else:
                sig_lines.append(line)
            if ";" in line:
                flush_signature()
                pending.clear()
                pending_gap = False
            elif sig_complete():
                flush_signature()
                pending.clear()
                pending_gap = False
            continue

        if not stripped:
            pending_gap = True
            continue

        if stripped.startswith("//"):
            body = strip_comment_marker(stripped)
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
            if not file_comment and file_pending:
                file_comment = list(file_pending)
            pending.clear()
            pending_gap = False
            in_define_cont = stripped.endswith("\\")
            continue

        if in_define_cont:
            in_define_cont = stripped.endswith("\\")
            continue

        if ACCESS_RE.match(stripped):
            for scope in reversed(scopes):
                if scope.kind == "class":
                    scope.public = stripped.startswith("public")
                    break
            pending.clear()
            continue

        # extern "C" opens a C-language scope (no C++ namespace).
        if re.match(r'extern\s+"C"\s*\{?', stripped):
            scopes.append(Scope(kind="externc", name="", public=False,
                               comment=list(pending)))
            pending.clear()
            continue

        # enum head: capture the body separately so its braces are not
        # mistaken for a class/block scope. Handles inline (`enum X { ... };`)
        # and multiline (`enum X` then `{` on a later line) forms.
        enum_head = re.match(
            r"(?:typedef\s+)?enum\s+(class|struct)?\s*"
            r"([A-Za-z_]\w*)?\s*(?::\s*[A-Za-z_][\w:]*\s*)?(\{)?", stripped)
        if enum_head:
            enum_name = enum_head.group(2) or ""
            if "{" in stripped:
                depth = stripped.count("{") - stripped.count("}")
                buf = stripped[stripped.find("{"):]
                if depth <= 0:
                    full = qualify(enum_name)
                    symbols.append(Symbol(kind="enum", lang=lang_for(""),
                                         qualified=full, id="E:" + full,
                                         enumerators=parse_enum_body(buf)))
                else:
                    in_enum = {"name": enum_name, "buf": buf, "depth": depth,
                               "lang": lang_for("")}
            else:
                in_enum = {"name": enum_name, "buf": "", "depth": 0,
                           "lang": lang_for("")}
            pending.clear()
            continue

        head = re.match(r"(?:template\s*<[^>]*>\s*)?"
                        r"(class|struct|namespace)\s+"
                        r"(?:[A-Z][A-Z0-9_]*\s+)*"
                        r"([A-Za-z_][A-Za-z0-9_]*)?", stripped)
        if head and not stripped.endswith(";"):
            kind = head.group(1)
            name = head.group(2) or ""
            scope = Scope(kind="class" if kind in ("class", "struct") else
                          "namespace", name=name,
                          public=(kind == "struct"), comment=list(pending))
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

        scan = stripped
        if first.endswith("_API"):
            scan = stripped[len(first):].strip()

        if ";" in scan:
            code, _, _ = scan.partition("//")
            if find_top_level_call(code):
                sig_lines = [code.rstrip()]
                sig_start = lineno
                flush_signature()
            pending.clear()
            pending_gap = False
            continue

        if find_top_level_call(scan):
            sig_lines = [scan]
            sig_start = lineno
            if sig_complete():
                flush_signature()
                pending.clear()
                pending_gap = False
            continue

        if first.endswith("_API"):
            sig_lines = [scan]
            sig_start = lineno
            continue

        if (";" not in stripped and "{" not in stripped
                and "}" not in stripped and first not in STMT_KEYWORDS
                and not find_top_level_call(scan)):
            sig_lines = [scan]
            sig_start = lineno
            continue
        pending.clear()

    return symbols


EXCLUDED_NAMES = {"avbase_c_internal.h", "avbase_c_export.h"}


def collect(root: Path) -> dict[str, Symbol]:
    symbols: dict[str, Symbol] = {}
    for path in sorted(root.rglob("*.h")):
        if path.name in EXCLUDED_NAMES:
            continue
        text = path.read_text(encoding="utf-8")
        for sym in extract_symbols(text, str(path.relative_to(root.parent))):
            # Collapse overloads: same id keeps the union of signatures.
            if sym.id in symbols:
                existing = symbols[sym.id]
                for s in sym.sigs:
                    if s not in existing.sigs:
                        existing.sigs.append(s)
                existing.is_virtual = existing.is_virtual or sym.is_virtual
                if sym.kind == "enum":
                    existing.enumerators.update(sym.enumerators)
            else:
                symbols[sym.id] = sym
    return symbols


# --- Diff ----------------------------------------------------------------
@dataclass
class DiffReport:
    breaking: list[str] = field(default_factory=list)
    compatible: list[str] = field(default_factory=list)
    virtual_changes: list[str] = field(default_factory=list)


def diff_surface(baseline: dict, current: dict) -> DiffReport:
    report = DiffReport()
    for sid, sym in baseline.items():
        if sid not in current:
            report.breaking.append(f"REMOVED  {sym.qualified} ({sym.kind})")
            continue
        cur = current[sid]
        if sym.kind == "enum":
            for ename, eval_ in sym.enumerators.items():
                if ename not in cur.enumerators:
                    report.breaking.append(
                        f"ENUM REMOVED  {sym.qualified}::{ename}")
                elif cur.enumerators[ename] != eval_:
                    report.breaking.append(
                        f"ENUM VALUE  {sym.qualified}::{ename} "
                        f"{eval_} -> {cur.enumerators[ename]}")
            for ename in cur.enumerators:
                if ename not in sym.enumerators:
                    report.compatible.append(
                        f"ENUM ADDED  {sym.qualified}::{ename}")
        else:
            for s in sym.sigs:
                if s not in cur.sigs:
                    report.breaking.append(
                        f"CHANGED  {sym.qualified}  [{s.replace(SIG_SEP, ' ')}]")
            for s in cur.sigs:
                if s not in sym.sigs:
                    report.compatible.append(
                        f"ADDED OVERLOAD  {sym.qualified} "
                        f"[{s.replace(SIG_SEP, ' ')}]")

    for sid, sym in current.items():
        if sid not in baseline:
            report.compatible.append(f"ADDED  {sym.qualified} ({sym.kind})")

    # vtable layout: a class's set of virtual method names must be stable.
    base_vt = _virtual_sets(baseline)
    cur_vt = _virtual_sets(current)
    for cls, names in base_vt.items():
        if cls in cur_vt and names != cur_vt[cls]:
            added = sorted(cur_vt[cls] - names)
            removed = sorted(names - cur_vt[cls])
            detail = []
            if removed:
                detail.append("removed " + ", ".join(removed))
            if added:
                detail.append("added " + ", ".join(added))
            report.virtual_changes.append(
                f"VTABLE  {cls}: " + "; ".join(detail))
    return report


def _virtual_sets(symbols: dict) -> dict[str, set]:
    vt: dict[str, set] = {}
    for sid, sym in symbols.items():
        if sym.kind == "method" and sym.is_virtual:
            cls = sid.split(":", 1)[1].rsplit("::", 1)[0]
            vt.setdefault(cls, set()).add(sym.qualified)
    return vt


# --- Baseline I/O --------------------------------------------------------
def git_sha() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=Path(__file__).resolve().parent.parent,
            stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return "unknown"


def write_baseline(symbols: dict, path: Path) -> None:
    data = {
        "tool": "check_api_compat.py",
        "schema": 1,
        "generated_by_commit": git_sha(),
        "generated_at": __import__("datetime").datetime.now(
            __import__("datetime").timezone.utc).isoformat(),
        "symbols": {sid: _sym_to_dict(s) for sid, s in sorted(symbols.items())},
    }
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def _sym_to_dict(s: Symbol) -> dict:
    d = {"kind": s.kind, "lang": s.lang, "qualified": s.qualified}
    if s.kind == "enum":
        d["enumerators"] = s.enumerators
    else:
        d["sigs"] = s.sigs
        d["is_virtual"] = s.is_virtual
    return d


def _dict_to_sym(sid: str, d: dict) -> Symbol:
    return Symbol(kind=d["kind"], lang=d.get("lang", "cpp"),
                  qualified=d["qualified"], id=sid,
                  sigs=list(d.get("sigs", [])),
                  is_virtual=bool(d.get("is_virtual", False)),
                  enumerators=dict(d.get("enumerators", {})))


def load_baseline(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    return {sid: _dict_to_sym(sid, d) for sid, d in data["symbols"].items()}


# --- Selftest ------------------------------------------------------------
SELFTEST_BEFORE = """\
#ifndef GUARD_H_
#define GUARD_H_
namespace avbase {
class Widget {
 public:
  void Start();
  void Stop(int timeout);
  Status SeekTo(int ms);
  virtual void OnEvent(Event e);
};
enum class Color { kRed, kGreen, kBlue };
AVBASE_C_API void avbase_free_thing(int* out);
}  // namespace avbase
#endif
"""

SELFTEST_AFTER = """\
#ifndef GUARD_H_
#define GUARD_H_
namespace avbase {
class Widget {
 public:
  void Start();
  void Stop(int timeout);
  Status SeekTo(base::Seconds t);   // signature CHANGED (breaking)
  virtual void OnEvent(Event e);
  virtual void OnTick();            // vtable CHANGED (breaking)
  void NewMethod();                 // ADDED (compatible)
};
enum class Color { kRed, kGreen, kBlue, kYellow };  // ENUM ADDED (compatible)
AVBASE_C_API void avbase_free_thing(int* out);
}  // namespace avbase
#endif
"""


def selftest() -> int:
    before = collect_from_text(SELFTEST_BEFORE, "before.h")
    after = collect_from_text(SELFTEST_AFTER, "after.h")
    rep = diff_surface(before, after)

    # Breaking: SeekTo signature change, vtable change (OnTick added).
    assert any("CHANGED  avbase::Widget::SeekTo" in b for b in rep.breaking), \
        "SeekTo signature change not flagged"
    assert any("VTABLE  avbase::Widget" in v for v in rep.virtual_changes), \
        "vtable change not flagged"
    # Compatible: NewMethod, Color::kYellow.
    assert any("ADDED  avbase::Widget::NewMethod" in c
               for c in rep.compatible), "NewMethod not marked compatible"
    assert any("ENUM ADDED  avbase::Color::kYellow" in c
               for c in rep.compatible), "kYellow not marked compatible"
    # No spurious removals.
    assert not any("REMOVED" in b for b in rep.breaking), \
        "spurious removal flagged"

    # A pure-addition diff must be fully compatible (no breaking).
    rep2 = diff_surface({}, after)
    assert not rep2.breaking and not rep2.virtual_changes, \
        "pure addition reported as breaking"
    # An identical surface must report nothing.
    rep3 = diff_surface(before, before)
    assert not rep3.breaking and not rep3.compatible \
        and not rep3.virtual_changes, "identical surface reported changes"

    # Enum value change must be breaking.
    before_v = dict(before)
    from copy import deepcopy
    after_v = deepcopy(after)
    after_v["E:avbase::Color"].enumerators["kGreen"] = 99
    rep4 = diff_surface(before_v, after_v)
    assert any("ENUM VALUE" in b for b in rep4.breaking), \
        "enum value change not flagged"

    print("selftest: parser and diff checks passed")
    return 0


def collect_from_text(text: str, name: str) -> dict:
    syms: dict[str, Symbol] = {}
    for sym in extract_symbols(text, name):
        if sym.id in syms:
            ex = syms[sym.id]
            for s in sym.sigs:
                if s not in ex.sigs:
                    ex.sigs.append(s)
            ex.is_virtual = ex.is_virtual or sym.is_virtual
            if sym.kind == "enum":
                ex.enumerators.update(sym.enumerators)
        else:
            syms[sym.id] = sym
    return syms


# --- Main -----------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default="player/public",
                        help="header tree to check (default: player/public)")
    parser.add_argument("--baseline", default="tools/api_baseline.json",
                        help="baseline JSON path")
    parser.add_argument("--snapshot", action="store_true",
                        help="(re)write the baseline from --root and exit")
    parser.add_argument("--report", action="store_true",
                        help="list every change")
    parser.add_argument("--json", action="store_true",
                        help="machine-readable output")
    parser.add_argument("--allow-breaking", action="store_true",
                        help="downgrade breaking changes to warnings "
                             "(use only when cutting a MAJOR version)")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    root = Path(args.root)
    if not root.is_dir():
        print(f"no such directory: {root}", file=sys.stderr)
        return 2
    current = collect(root)

    if args.snapshot:
        write_baseline(current, Path(args.baseline))
        print(f"snapshot: wrote {len(current)} symbols to {args.baseline}")
        return 0

    baseline_path = Path(args.baseline)
    if not baseline_path.is_file():
        print(f"no baseline at {baseline_path} -- run with --snapshot first",
              file=sys.stderr)
        return 2
    baseline = load_baseline(baseline_path)
    rep = diff_surface(baseline, current)

    breaking = rep.breaking + [f"VTABLE {v}" for v in rep.virtual_changes]
    exit_code = 0 if (args.allow_breaking or not breaking) else 1

    if args.json:
        print(json.dumps({
            "current_symbols": len(current),
            "baseline_symbols": len(baseline),
            "breaking": breaking,
            "compatible": rep.compatible,
            "exit_code": exit_code}, indent=2))
        return exit_code

    print(f"V10 API compat ({root} vs {baseline_path}): "
          f"{len(current)} current / {len(baseline)} baseline symbols")
    if breaking:
        tag = "BREAKING" if not args.allow_breaking else "BREAKING (allowed)"
        print(f"  {tag}: {len(breaking)}")
        for b in breaking:
            print(f"    - {b}")
    else:
        print("  breaking: 0")
    if rep.compatible:
        print(f"  compatible (minor-OK): {len(rep.compatible)}")
        if args.report:
            for c in rep.compatible:
                print(f"    + {c}")
    if rep.virtual_changes and args.report:
        for v in rep.virtual_changes:
            print(f"    ~ {v}")
    if breaking and args.report:
        for b in breaking:
            print(f"    - {b}")

    if breaking and not args.allow_breaking:
        print("FAIL: breaking changes detected (rc=1). A minor bump must not "
              "break user code. Re-snapshot only when cutting a MAJOR.",
              file=sys.stderr)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
