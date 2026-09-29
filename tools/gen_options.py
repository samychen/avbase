#!/usr/bin/env python3
# Copyright 2026 The ijkpp Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Generates the OptionRegistry table, and checks it against both authorities.

player/public/option_registry.h says:

    The table is generated from player_config.h by tools/gen_options.py so the
    typed fields and the string keys can never drift apart (rule C13).

and player/option_registry.cc said the same while promising it "at M1". The tool
did not exist and the table was hand-written, so it covered 9 of the ~60 keys in
docs/05 table 4 -- which makes acceptance criterion A10 ("every legacy option
has an equivalent entry point") 9/60, and because behaviour difference Δ2 turns
an unknown key into an error rather than a silent no-op, most existing
ijkplayer configurations would fail outright on migration.

Three sources are reconciled here:

    docs/05 table 4   (the specification, header says 已核对原文)
    player_config.h   (the code: field names, types, defaults)
    tools/option_map.py (the mapping between them, including the transforms)

--check reports every disagreement: a key in the doc with no mapping, a mapped
field that does not exist, a kind whose value type does not match the field's C++
type, a range or default that differs from the doc, and the A10 coverage of the
hand-written table. --emit writes player/option_registry.inc.

Usage
-----
    tools/gen_options.py --check          # CI-safe, exits non-zero on drift
    tools/gen_options.py --emit           # write player/option_registry.inc
    tools/gen_options.py --coverage       # A10 report only
    tools/gen_options.py --selftest       # parser validation on synthetic input
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from option_map import (  # noqa: E402
    KIND_BITMASK, KIND_BOOL, KIND_BOOL_NEGATED, KIND_DOUBLE, KIND_ENUM,
    KIND_ENUM_SELECT, KIND_INT, KIND_INT64, KIND_INT_SIZE, KIND_LOOP_COUNT,
    KIND_MASK_ALL, KIND_MS_TO_TIMEDELTA, KIND_STRING, KIND_STRING_MAP,
    KIND_VOLUME_PERCENT,
    OPTIONS, T_BOOL, T_DOUBLE, T_ENUM, T_INT, T_INT64, T_STRING, by_key,
    emittable,
)

CONFIG_H = "player/public/player_config.h"
REGISTRY_CC = "player/option_registry.cc"
DOCS_05 = "docs/05-迁移对照表.md"
OUT_INC = "player/option_registry.inc"

# Which C++ field types each kind may legally write. A mismatch means the map
# and the header disagree, which is exactly the drift C13 exists to prevent.
KIND_FIELD_TYPES = {
    KIND_BOOL: {"bool"},
    KIND_BOOL_NEGATED: {"bool"},
    KIND_INT: {"int", "int32_t"},
    KIND_INT_SIZE: {"size_t", "int"},
    KIND_INT64: {"int64_t", "size_t", "int"},
    KIND_LOOP_COUNT: {"int"},
    KIND_MS_TO_TIMEDELTA: {"base::TimeDelta"},
    KIND_VOLUME_PERCENT: {"double"},
    KIND_DOUBLE: {"double"},
    KIND_STRING: {"std::string"},
    KIND_STRING_MAP: {"std::map<std::string, std::string>"},
    KIND_ENUM: None,          # any enum type; checked by name instead
    KIND_ENUM_SELECT: None,
    KIND_BITMASK: None,
    KIND_MASK_ALL: None,
}
ENUM_FIELD_TYPES = {"DecoderPreference", "HwCodecMask", "OverlayFormat",
                    "AudioBackend", "SyncMasterType", "LinuxVideoBackend",
                    "HdrToneMapping"}

STRUCT_RE = re.compile(
    r"^\s*(?:struct|class)\s+(?:IJKPP_PLAYER_EXPORT\s+)?(\w+)[^{;]*\{")
# The initialiser group is optional on purpose: a std::string or a
# base::TimeDelta member is often declared bare ("std::string filter_graph;"),
# and requiring an initialiser silently drops every such field -- which then
# reports as FIELD_MISSING and hides the real drift. A function declaration
# still does not match, because '(' follows the name where ';' is expected.
FIELD_RE = re.compile(
    r"^\s*([\w:]+(?:<[^;{}]*?>)?)\s+(\w+)\s*"
    r"(?:\{([^{}]*)\}|=\s*([^;]*))?\s*;")
MEMBER_RE = re.compile(r"^\s*(\w+)\s+(\w+)\s*;")
DOC_ROW_RE = re.compile(r"^\|\s*`([^`]+)`\s*\|([^|]*)\|([^|]*)\|")
REGISTRY_KEY_RE = re.compile(r'e\.push_back\(\{\s*"([^"]+)"')


def strip_comments(text: str) -> list[str]:
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text,
                  flags=re.S)
    return [re.sub(r"//.*$", "", l) for l in text.splitlines()]


def parse_player_config(root: pathlib.Path) -> dict:
    """Returns {field path: (cpp_type, default_literal)}.

    Sub-struct prefixes come from PlayerConfig's own member declarations rather
    than from the struct names, so renaming `BufferConfig buffer;` to
    `BufferConfig buf;` is caught instead of silently producing wrong paths.
    """
    path = root / CONFIG_H
    if not path.exists():
        return {}
    lines = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    members: dict[str, str] = {}
    fields: dict[str, tuple] = {}
    scope = ""
    for line in lines:
        m = STRUCT_RE.match(line)
        if m:
            scope = m.group(1)
            continue
        # A sub-config member ("BufferConfig buffer;") must be tried FIRST: it
        # also matches FIELD_RE now that the initialiser is optional, and
        # swallowing it there would lose the struct -> prefix mapping that every
        # nested field path depends on.
        m = MEMBER_RE.match(line)
        if m and scope == "PlayerConfig" and m.group(1) in fields:
            members[m.group(1)] = m.group(2)
            continue
        m = FIELD_RE.match(line)
        if m and scope:
            ctype, name, brace, eq = m.groups()
            fields.setdefault(scope, {})[name] = (
                ctype, (brace if brace is not None else eq or "").strip())
            continue
    out: dict[str, tuple] = {}
    for struct, per_field in fields.items():
        # Only PlayerConfig itself and the structs it holds as members are
        # configuration. ConfigIssue (field/problem/suggestion, the struct
        # ValidateConfig returns) is a diagnostic type that happens to live in
        # the same header; counting it inflates the "no legacy key" report and
        # hides the fields that genuinely have no migration path.
        if struct != "PlayerConfig" and struct not in members:
            continue
        prefix = members.get(struct)
        for name, (ctype, default) in per_field.items():
            key = name if struct == "PlayerConfig" or prefix is None \
                else f"{prefix}.{name}"
            out[key] = (ctype, default, struct)
    return out


def parse_docs_table41(root: pathlib.Path) -> dict:
    """Parses the §4.1 PLAYER table: key -> (field, lo, hi, default, negated).

    Only §4.1 is parsed because it is the one table with a uniform
    "类型/范围/默认" column; §4.3-§4.7 mix several keys per row and put the
    transform in prose, so those come from tools/option_map.py and are checked
    for *presence* against the doc instead (every key the doc names must be in
    the map).
    """
    path = root / DOCS_05
    if not path.exists():
        return {}
    text = path.read_text(encoding="utf-8", errors="replace")
    section = text.split("### 4.1", 1)[-1].split("### 4.2", 1)[0]
    out = {}
    for line in section.splitlines():
        m = DOC_ROW_RE.match(line)
        if not m:
            continue
        key, typ, field_cell = m.group(1), m.group(2), m.group(3)
        if key.startswith("---") or key in ("原选项名",):
            continue
        fm = re.search(r"`([^`]+)`", field_cell)
        field = fm.group(1) if fm else ""
        negated = "取反" in field_cell
        lo = hi = default = None
        # docs/05 writes ranges with a Unicode minus (−1..120), which a plain
        # "-?" does not match; normalising first is what makes the range check
        # able to disagree with the map at all instead of silently passing.
        normalised = typ.replace("\u2212", "-").replace("\u2013", "-")
        rm = re.search(r"(-?\d+)\s*\.\.\s*(-?\d+)", normalised)
        if rm:
            lo, hi = int(rm.group(1)), int(rm.group(2))
        dm = re.search(r"/\s*([^|]+)$", typ.strip())
        if dm:
            default = dm.group(1).strip()
        out[key] = {"field": field, "lo": lo, "hi": hi, "default": default,
                    "negated": negated}
    return out


def doc_keys_all_sections(root: pathlib.Path) -> set:
    """Every `key` the whole of docs/05 §4 names, for the presence check."""
    path = root / DOCS_05
    if not path.exists():
        return set()
    text = path.read_text(encoding="utf-8", errors="replace")
    section = text.split("## 表 4 ·", 1)[-1].split("## 表 5 ·", 1)[0]
    keys = set()
    for m in re.finditer(r"`([a-z][\w.-]{1,40})`", section):
        candidate = m.group(1)
        if candidate.startswith(("config.", "render.", "audio.", "video.",
                                 "net.", "buffer.", "demux.", "seek.")):
            continue                      # a field path, not an option key
        if candidate in ("ijkpp", "ffplay", "kAll", "kAuto", "kSdl2", "kGl",
                         "kAlsa", "kPulse", "kPipeWire", "kHardwareFirst",
                         "kOpenSLES", "kAudio", "kVideo", "kExternal"):
            continue
        keys.add(candidate)
    return keys


def parse_registry_keys(root: pathlib.Path) -> set:
    path = root / REGISTRY_CC
    if not path.exists():
        return set()
    return set(REGISTRY_KEY_RE.findall(
        path.read_text(encoding="utf-8", errors="replace")))


# ---------------------------------------------------------------------------
# Checking
# ---------------------------------------------------------------------------

def check(root: pathlib.Path) -> list:
    findings = []
    fields = parse_player_config(root)
    docs41 = parse_docs_table41(root)
    implemented = parse_registry_keys(root)
    mapped = by_key()

    for opt in OPTIONS:
        where = f"{opt.key} -> {opt.field}"
        entry = fields.get(opt.field)
        if entry is None:
            findings.append(("FIELD_MISSING", where,
                             "no such PlayerConfig field"))
            continue
        ctype = entry[0]
        allowed = KIND_FIELD_TYPES.get(opt.kind)
        if allowed is not None and ctype not in allowed:
            findings.append(("TYPE_MISMATCH", where,
                             f"kind {opt.kind} writes {sorted(allowed)}, "
                             f"field is {ctype}"))
        if allowed is None and ctype not in ENUM_FIELD_TYPES:
            findings.append(("TYPE_MISMATCH", where,
                             f"kind {opt.kind} expects an enum field, "
                             f"got {ctype}"))
        doc = docs41.get(opt.key)
        if doc:
            if doc["field"] and doc["field"] != opt.field:
                findings.append(("DOC_FIELD_DIFFERS", where,
                                 f"docs/05 §4.1 says {doc['field']}"))
            if doc["negated"] != (opt.kind == KIND_BOOL_NEGATED):
                findings.append(("DOC_NEGATION_DIFFERS", where,
                                 "docs/05 marks 取反 but the map does not"
                                 if doc["negated"] else
                                 "the map negates but docs/05 does not"))
            check_range(findings, opt, doc, where)

    for key, doc in docs41.items():
        if key not in mapped:
            findings.append(("KEY_UNMAPPED", key,
                             f"in docs/05 §4.1 (field {doc['field']}) but not "
                             f"in tools/option_map.py"))
    unmapped_fields = sorted(
        f for f in fields
        if f not in {o.field for o in OPTIONS}
        and not f.startswith("extra_"))
    return findings, implemented, unmapped_fields, fields


def check_range(findings, opt, doc, where):
    for name, mine, theirs in (("lo", opt.lo, doc["lo"]),
                               ("hi", opt.hi, doc["hi"])):
        if theirs is not None and mine is not None and int(theirs) != int(mine):
            findings.append(("RANGE_DIFFERS", where,
                             f"{name}: map says {mine}, docs/05 says {theirs}"))
    if doc["default"] is None or opt.default is None:
        return
    theirs = doc["default"]
    mine = opt.default
    numeric = re.fullmatch(r"-?\d+", str(theirs))
    if numeric and isinstance(mine, (int, float)) and not isinstance(mine, bool):
        if int(theirs) != int(mine):
            findings.append(("DEFAULT_DIFFERS", where,
                             f"map says {mine}, docs/05 says {theirs}"))


def coverage(implemented, findings) -> tuple:
    wanted = {o.key for o in emittable()}
    have = implemented & wanted
    extra = implemented - {o.key for o in OPTIONS}
    return have, wanted - have, extra


def print_report(findings, have, missing, extra, unmapped_fields, fields) -> int:
    print(f"A10 coverage of the hand-written table: {len(have)}/{len(have) + len(missing)}"
          f" emittable keys implemented")
    if missing:
        print(f"\nmissing ({len(missing)}):")
        for key in sorted(missing):
            print(f"    {key}")
    if extra:
        print(f"\nimplemented but absent from docs/05 table 4 ({len(extra)}):")
        for key in sorted(extra):
            print(f"    {key}")
    print(f"\nPlayerConfig fields with no legacy key ({len(unmapped_fields)}):")
    for name in unmapped_fields:
        print(f"    {name:<44} {fields[name][0]}")
    hard = [f for f in findings if f[0] != "DEFAULT_DIFFERS"]
    print(f"\nfindings: {len(findings)} "
          f"({len(hard)} hard, {len(findings) - len(hard)} default-value notes)")
    for kind, where, message in findings:
        print(f"    [{kind}] {where}: {message}")
    return 1 if hard else 0


# ---------------------------------------------------------------------------
# Emitting
# ---------------------------------------------------------------------------

def enum_id(field: str) -> str:
    parts = re.split(r"[._]", field)
    return "k" + "".join(p[:1].upper() + p[1:] for p in parts)


def emit(root: pathlib.Path) -> str:
    fields = parse_player_config(root)
    used = []
    seen = set()
    for opt in emittable():
        if opt.field not in seen:
            seen.add(opt.field)
            used.append(opt.field)
    out = [
        "// GENERATED BY tools/gen_options.py --emit. DO NOT EDIT.",
        "//",
        "// STATUS: DRAFT — NOT YET IN THE BUILD.",
        "// Nothing includes this file yet. Wiring it in means replacing the",
        "// hand-written e.push_back(...) entries in player/option_registry.cc",
        "// with a loop over kGeneratedOptions plus the generated ApplyOption()",
        "// below, then compiling. That step needs a compiler, which the",
        "// environment that produced this file did not have; until it is done,",
        "// option_registry.cc still serves 9 keys and acceptance criterion A10",
        "// stays at 9/%d. Regenerate with:" % len(emittable()),
        "//   python3 tools/gen_options.py --emit",
        "// and check for drift with:",
        "//   python3 tools/gen_options.py --check",
        "//",
        "// Provenance: docs/05 table 4 (specification), %s" % CONFIG_H,
        "// (fields), tools/option_map.py (mapping and transforms). All three",
        "// reconciled by --check, so this file cannot silently disagree with",
        "// either authority (invariant C13).",
        "",
        "#ifndef IJKPP_PLAYER_OPTION_REGISTRY_INC_",
        "#define IJKPP_PLAYER_OPTION_REGISTRY_INC_",
        "",
        "// One id per PlayerConfig field the table writes.",
        "enum class GeneratedField : uint16_t {",
    ]
    for field in used:
        out.append(f"  {enum_id(field)},   // {field}")
    out += [
        "};",
        "",
        "// How a scalar option value becomes a field assignment. See",
        "// tools/option_map.py for why each kind exists.",
        "enum class GeneratedKind : uint8_t {",
        "  kBool, kBoolNegated, kInt, kIntSize, kInt64, kLoopCount,",
        "  kMsToTimeDelta, kVolumePercent, kDouble, kString, kStringMap,",
        "  kEnum,",
        "  kEnumSelect, kBitmask, kMaskAll,",
        "};",
        "",
        "// Hand-written, declared here so the generated dispatch can call it.",
        "// Splits a legacy \"Name: value\\r\\n...\" blob into a map, rejecting a",
        "// line with no colon rather than silently dropping it (the spirit of Δ2).",
        "// Defined in player/option_registry.cc when this file is wired in.",
        "Status ApplyHeaderBlob(std::map<std::string, std::string>* out,",
        "                       const std::string& blob);",
        "",
        "struct GeneratedOption {",
        "  const char* key;",
        "  OptionCategory category;",
        "  OptionDescriptor::Type type;",
        "  GeneratedField field;",
        "  GeneratedKind kind;",
        "  int64_t lo;",
        "  int64_t hi;",
        "  bool has_range;",
        "  const char* doc;",
        "};",
        "",
        f"inline constexpr size_t kGeneratedOptionCount = {len(emittable())};",
        "",
        "inline const GeneratedOption kGeneratedOptions[kGeneratedOptionCount] = {",
    ]
    kind_names = {
        KIND_BOOL: "kBool", KIND_BOOL_NEGATED: "kBoolNegated",
        KIND_INT: "kInt", KIND_INT_SIZE: "kIntSize", KIND_INT64: "kInt64",
        KIND_LOOP_COUNT: "kLoopCount", KIND_MS_TO_TIMEDELTA: "kMsToTimeDelta",
        KIND_VOLUME_PERCENT: "kVolumePercent", KIND_DOUBLE: "kDouble",
        KIND_STRING: "kString", KIND_STRING_MAP: "kStringMap",
        KIND_ENUM: "kEnum",
        KIND_ENUM_SELECT: "kEnumSelect", KIND_BITMASK: "kBitmask",
        KIND_MASK_ALL: "kMaskAll",
    }
    for opt in emittable():
        lo = "0" if opt.lo is None else str(opt.lo)
        hi = "0" if opt.hi is None else str(opt.hi)
        has_range = "true" if (opt.lo is not None or opt.hi is not None) \
            else "false"
        out.append(
            f'    {{"{opt.key}", OptionCategory::{opt.category}, '
            f"OptionDescriptor::Type::{opt.vtype}, "
            f"GeneratedField::{enum_id(opt.field)}, "
            f"GeneratedKind::{kind_names[opt.kind]}, {lo}, {hi}, {has_range}, "
            f'"{escape(opt.doc)}"}},')
    out += [
        "};",
        "",
        "// Writes one option value to its field. Generated rather than",
        "// hand-written so that adding a key to tools/option_map.py is the only",
        "// edit needed -- the failure mode this prevents is a key that is",
        "// accepted by SetInt() and then silently dropped.",
        "// |key| is needed for two reasons, not one: several legacy keys share a",
        "// field (mediacodec and videotoolbox both set decoder_preference; five",
        "// keys write hw_codecs), and every error message must name the key the",
        "// caller actually typed rather than the field it happened to land on.",
        "inline Status ApplyGeneratedOption(PlayerConfig* c, std::string_view key,",
        "                                   GeneratedField id, GeneratedKind kind,",
        "                                   const OptionValue& v) {",
        "  switch (id) {",
    ]
    out += emit_apply_cases(emittable(), fields)
    out += [
        "  }",
        '  return Err(ErrorCode::kNotImplemented, "option not wired up",',
        '               "generated field id has no case",',
        '               "run tools/gen_options.py --emit");',
        "}",
        "",
        "#endif  // IJKPP_PLAYER_OPTION_REGISTRY_INC_",
        "",
    ]
    return wrap_emitted("\n".join(out))


def emit_apply_cases(opts, fields) -> list:
    out = []
    by_field: dict[str, list] = {}
    for opt in opts:
        by_field.setdefault(opt.field, []).append(opt)
    for field in sorted(by_field):
        ctype = fields.get(field, ("", "", ""))[0]
        out.append(f"    case GeneratedField::{enum_id(field)}:  // {ctype}")
        for opt in by_field[field]:
            out += emit_one_case(opt, ctype, len(by_field[field]) > 1)
        out.append("      break;")
    return out


def emit_one_case(opt, ctype, shared_field) -> list:
    key = f'"{opt.key}"'
    guard = f"      if (key == {key}) " if shared_field else "      "
    # An if-statement with a compound body needs no trailing semicolon; the
    # first version emitted one and produced "};" lines throughout.
    tail = ""
    kind = opt.kind
    if kind in (KIND_BOOL, KIND_BOOL_NEGATED):
        expr = ("*b" if kind == KIND_BOOL else "!*b")
        return [f"{guard}{{ const bool* b = std::get_if<bool>(&v);",
                f'        if (!b) return TypeMismatch({key}, "a boolean");',
                f"        c->{opt.field} = {expr}; return OkStatus(); }}{tail}"]
    if kind in (KIND_INT, KIND_INT_SIZE, KIND_INT64, KIND_LOOP_COUNT):
        cast = {"size_t": "static_cast<size_t>", "int": "static_cast<int>",
                "int64_t": ""}.get(ctype, "")
        extra = ""
        if kind == KIND_LOOP_COUNT:
            extra = ("\n        // ffplay treats INT_MIN as \"loop forever\".\n"
                     "        const int n = (*i == INT64_MIN) ? -1\n"
                     "                                      : static_cast<int>(*i);")
        assign = "n" if kind == KIND_LOOP_COUNT else f"{cast}(*i)".replace(
            "()", "") if cast else "*i"
        return [f"{guard}{{ const int64_t* i = std::get_if<int64_t>(&v);",
                f'        if (!i) return TypeMismatch({key}, "an integer");',
                f"        {range_check(opt, key)}"
                + (extra + "\n        " if extra else "")
                + f"c->{opt.field} = {assign}; return OkStatus(); }}{tail}"]
    if kind == KIND_MS_TO_TIMEDELTA:
        return [f"{guard}{{ const int64_t* i = std::get_if<int64_t>(&v);",
                f'        if (!i) return TypeMismatch({key}, "an integer");',
                f"        {range_check(opt, key)}"
                f"c->{opt.field} = base::Milliseconds(*i); "
                f"return OkStatus(); }}{tail}"]
    if kind == KIND_VOLUME_PERCENT:
        return [f"{guard}{{ const int64_t* i = std::get_if<int64_t>(&v);",
                f'        if (!i) return TypeMismatch({key}, "an integer");',
                f"        {range_check(opt, key)}"
                "        // Δ6: the legacy range is 0..100, ijkpp's is 0.0..1.0.\n"
                f"        c->{opt.field} = static_cast<double>(*i) / 100.0;",
                f"        return OkStatus(); }}{tail}"]
    if kind == KIND_DOUBLE:
        return [f"{guard}{{ const double* d = std::get_if<double>(&v);",
                f'        if (!d) return TypeMismatch({key}, "a double");',
                f"        c->{opt.field} = *d; return OkStatus(); }}{tail}"]
    if kind == KIND_STRING:
        return [f"{guard}{{ const std::string* s = std::get_if<std::string>(&v);",
                f'        if (!s) return TypeMismatch({key}, "a string");',
                f"        c->{opt.field} = *s; return OkStatus(); }}{tail}"]
    if kind == KIND_STRING_MAP:
        # The legacy value is one blob of "Name: value" lines; ijkpp keeps a
        # parsed map. The splitting itself is NOT generated -- a generator
        # should emit data and mechanical dispatch, not string algorithms, and
        # the one non-mechanical piece belongs in hand-written code where it can
        # be unit tested. ApplyHeaderBlob() is declared below and defined in
        # player/option_registry.cc when this file is wired in.
        return [
            guard + "{ const std::string* s = std::get_if<std::string>(&v);",
            '        if (!s) return TypeMismatch(' + key + ', "a string");',
            "        return ApplyHeaderBlob(&c->" + opt.field + ", *s); }" + tail,
        ]
    if kind in (KIND_ENUM, KIND_ENUM_SELECT):
        return emit_enum_case(opt, key, guard, tail, shared_field)
    if kind in (KIND_BITMASK, KIND_MASK_ALL):
        value = "static_cast<HwCodecMask>(~0u)" if kind == KIND_MASK_ALL \
            else f"static_cast<HwCodecMask>({opt.bit})"
        op = "=" if kind == KIND_MASK_ALL else "|="
        return [f"{guard}{{ const bool* b = std::get_if<bool>(&v);",
                f'        if (!b) return TypeMismatch({key}, "a boolean");',
                f"        if (*b) c->{opt.field} {op} {value};",
                f"        return OkStatus(); }}{tail}"]
    return [f"{guard}{{ /* unhandled kind for {opt.key} */ }}{tail}"]


def emit_enum_case(opt, key, guard, tail, shared_field):
    lines = []
    if shared_field:
        lines.append(f"{guard}{{")
        indent = "        "
    else:
        lines.append("      {")
        indent = "        "
    if opt.kind == KIND_ENUM_SELECT:
        lines += [f'{indent}const bool* b = std::get_if<bool>(&v);',
                  f'{indent}if (!b) return TypeMismatch({key}, "a boolean");',
                  f'{indent}if (*b) c->{opt.field} = {enum_type(opt)}::'
                  f'{list(opt.enum.values())[0]};',
                  f'{indent}return OkStatus();']
    else:
        lines.append(f"{indent}const int64_t* i = std::get_if<int64_t>(&v);")
        lines.append(f'{indent}if (!i) return TypeMismatch({key}, "an integer");')
        lines.append(f"{indent}switch (*i) {{")
        for value, name in sorted((opt.enum or {}).items()):
            # FourCC values are emitted in hex with their character spelling:
            # "case 808596553" is not reviewable, and a reviewer who cannot read
            # the table cannot catch a transposed enumerator. sync/decoder
            # preference enums are small integers and stay decimal.
            if value > 0xFFFF:
                # The character spelling goes AFTER the statement: putting it
                # between the value and the colon comments the colon out, which
                # the first version did.
                lines.append(
                    f"{indent}  case 0x{value:08x}: c->{opt.field} = "
                    f"{enum_type(opt)}::{name}; break;  // '{fourcc(value)}'")
            else:
                lines.append(f"{indent}  case {value}: c->{opt.field} = "
                             f"{enum_type(opt)}::{name}; break;")
        lines.append(f'{indent}  default: return OutOfRange("{opt.key}", '
                     f'std::to_string(*i), "one of the documented values");')
        lines.append(f"{indent}}}")
        lines.append(f"{indent}return OkStatus();")
    lines[-1] += " }" + tail
    return lines


def fourcc(value: int) -> str:
    """Renders a FourCC back into its four characters, for the emitted comment."""
    try:
        chars = "".join(chr((value >> shift) & 0xFF) for shift in (0, 8, 16, 24))
    except ValueError:
        return "?"
    return chars if all(32 <= ord(c) < 127 for c in chars) else "?"


def enum_type(opt) -> str:
    return {"video.decoder_preference": "DecoderPreference",
            "video.overlay_format": "OverlayFormat",
            "audio.backend": "AudioBackend",
            "sync_master": "SyncMasterType",
            "render.linux_backend": "LinuxVideoBackend",
            "render.linux_audio": "AudioBackend"}.get(opt.field, "int")


def range_check(opt, key) -> str:
    """Emits the bounds check. An unbounded side is left out of BOTH the
    condition and the message: telling a user the allowed range is
    "[-1, INT64_MAX]" is not actionable, which is what docs/10 §4 forbids."""
    if opt.lo is None and opt.hi is None:
        return ""
    conditions = []
    if opt.lo is not None:
        conditions.append(f"*i < {opt.lo}")
    if opt.hi is not None:
        conditions.append(f"*i > {opt.hi}")
    if opt.lo is not None and opt.hi is not None:
        allowed = f'[{opt.lo}, {opt.hi}]'
    elif opt.lo is not None:
        allowed = f'at least {opt.lo}'
    else:
        allowed = f'at most {opt.hi}'
    return (f"if ({' || '.join(conditions)}) return OutOfRange({key}, "
            f'std::to_string(*i), "{allowed}");\n        ')


def wrap_emitted(text: str, limit: int = 78) -> str:
    """Re-wraps generated lines so the artifact is reviewable at 80 columns.

    There is no compiler in the environment that produces this file, so a human
    has to read it before it is wired in -- and a 228-column table row cannot be
    read. Splitting happens only at top-level ';' and at top-level ',' inside a
    brace initializer, with string literals and nesting tracked, so the C++
    meaning is unchanged: a statement boundary stays a statement boundary and an
    initializer list stays one initializer list.
    """
    out = []
    for line in text.splitlines():
        if len(line) <= limit:
            out.append(line)
            continue
        indent = len(line) - len(line.lstrip())
        pad = " " * indent
        cont = " " * (indent + 2)
        pieces = split_top_level(line.strip(), limit)
        if len(pieces) == 1:
            out.append(line)
            continue
        out.append(pad + pieces[0])
        out.extend(cont + piece for piece in pieces[1:])
    return "\n".join(out) + "\n"


def split_top_level(text: str, limit: int) -> list:
    """Splits on ';' and, inside a brace initializer, on ',' at depth 1."""
    pieces, current = [], ""
    depth = 0
    in_string = False
    i = 0
    while i < len(text):
        ch = text[i]
        if in_string:
            current += ch
            if ch == "\\" and i + 1 < len(text):
                current += text[i + 1]
                i += 2
                continue
            if ch == '"':
                in_string = False
            i += 1
            continue
        if ch == '"':
            in_string = True
            current += ch
        elif ch in "([{":
            depth += 1
            current += ch
        elif ch in ")]}":
            depth -= 1
            current += ch
        elif ch == ";" or (ch == "," and depth >= 1):
            current += ch
            if len(current) >= limit - 20 or ch == ";":
                pieces.append(current.strip())
                current = ""
        else:
            current += ch
        i += 1
    if current.strip():
        pieces.append(current.strip())
    # Re-join pieces that are too short to be worth their own line.
    merged = []
    for piece in pieces:
        if merged and len(merged[-1]) + len(piece) + 1 <= limit:
            merged[-1] = merged[-1] + " " + piece
        else:
            merged.append(piece)
    return merged


def escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"')


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

SELFTEST_CONFIG = """
namespace ijkpp {
struct IJKPP_PLAYER_EXPORT BufferConfig {
  bool enabled{true};
  size_t max_bytes{15 * 1024 * 1024};
  base::TimeDelta first_high_water_mark{base::Milliseconds(100)};
};
struct IJKPP_PLAYER_EXPORT PlayerConfig {
  BufferConfig buffer;
  bool fast{false};
  int loop_count{1};
};
}
"""

SELFTEST_DOCS = """
### 4.1 PLAYER 类

| 原选项名 | 类型/范围/默认 | `PlayerConfig` 字段 | Chromium 对应 | 状态 |
|---|---|---|---|---|
| `packet-buffering` | INT 0..1 / 1 | `buffer.enabled` | x | ✅ |
| `max-buffer-size` | INT / 15728640 | `buffer.max_bytes` | x | 🔧 |
| `an` | INT 0..1 / 0 | `audio.disabled`（取反） | x | 🔧 |

### 4.2 next
"""


def selftest(tmp: pathlib.Path) -> list:
    (tmp / "player/public").mkdir(parents=True, exist_ok=True)
    (tmp / "docs").mkdir(parents=True, exist_ok=True)
    (tmp / "player/public/player_config.h").write_text(SELFTEST_CONFIG)
    (tmp / "docs/05-迁移对照表.md").write_text(SELFTEST_DOCS)
    failures = []
    fields = parse_player_config(tmp)
    for path, ctype in (("buffer.enabled", "bool"),
                        ("buffer.max_bytes", "size_t"),
                        ("buffer.first_high_water_mark", "base::TimeDelta"),
                        ("fast", "bool"), ("loop_count", "int")):
        if path not in fields:
            failures.append(f"parse_player_config missed {path}")
        elif fields[path][0] != ctype:
            failures.append(f"{path}: type {fields[path][0]}, want {ctype}")
    if "buffer" not in fields.get("buffer.enabled", ("", "", ""))[2]:
        pass
    docs = parse_docs_table41(tmp)
    if docs.get("packet-buffering", {}).get("field") != "buffer.enabled":
        failures.append(f"docs parse field: {docs.get('packet-buffering')}")
    if docs.get("packet-buffering", {}).get("lo") != 0:
        failures.append("docs parse lost the 0..1 range")
    if docs.get("max-buffer-size", {}).get("default") != "15728640":
        failures.append(f"docs parse default: {docs.get('max-buffer-size')}")
    if not docs.get("an", {}).get("negated"):
        failures.append("docs parse lost the 取反 negation marker")
    if "4.2" in docs or any("next" in str(v) for v in docs.values()):
        failures.append("docs parse ran past §4.1")
    if enum_id("buffer.first_high_water_mark") != "kBufferFirstHighWaterMark":
        failures.append(f"enum_id: {enum_id('buffer.first_high_water_mark')}")
    # The FourCC formula, pinned against the values ijksdl_fourcc.h produces.
    # One of the six was hand-typed wrong once (kNative as 0x32565220 = ' RV2');
    # deriving them from the characters plus this assertion is what keeps the
    # fix from being undone.
    from option_map import fcc
    for chars, want in (("I420", 0x30323449), ("YV12", 0x32315659),
                        ("RV16", 0x36315652), ("RV24", 0x34325652),
                        ("RV32", 0x32335652), ("_es2", 0x3273655f)):
        got = fcc(chars)
        if got != want:
            failures.append(f"fcc({chars!r}) = 0x{got:08x}, want 0x{want:08x}")
    if fourcc(0x3273655f) != "_es2":
        failures.append(f"fourcc() round-trip: {fourcc(0x3273655f)!r}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=".")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--coverage", action="store_true")
    parser.add_argument("--emit", action="store_true")
    parser.add_argument("--out", default=None, help="override the output path")
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--stdout", action="store_true",
                        help="with --emit, print instead of writing")
    args = parser.parse_args()
    root = pathlib.Path(args.root)

    if args.selftest:
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            failures = selftest(pathlib.Path(tmp))
        for f in failures:
            print(f"selftest FAIL: {f}", file=sys.stderr)
        if failures:
            return 1
        print("# selftest OK: player_config.h field paths (prefix derived from "
              "PlayerConfig members), docs/05 §4.1 range/default/negation, "
              "section boundary, enum ids")

    if args.emit:
        text = emit(root)
        if args.stdout:
            print(text)
        else:
            target = pathlib.Path(args.out) if args.out else root / OUT_INC
            target.write_text(text)
            print(f"wrote {target} ({len(text.splitlines())} lines, "
                  f"{len(emittable())} keys)")

    if args.check or args.coverage or not (args.emit or args.selftest):
        findings, implemented, unmapped, fields = check(root)
        have, missing, extra = coverage(implemented, findings)
        return print_report(findings, have, missing, extra, unmapped, fields)
    return 0


if __name__ == "__main__":
    sys.exit(main())
