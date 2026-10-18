#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Converts the Doxygen XML of the SDK surface into docs/API.md.

Run by tools/gen_api_docs.sh; not meant to be run standalone (it assumes
build/api-docs/xml exists and doxygen has already run warning-free).

Layout: one section per public header, in path order. Inside a header:
classes/structs (with their public members), then file-scope enums, typedefs,
functions and variables, in source order. Every signature is taken from the
XML, so the document cannot drift from the headers unless the headers drift
from the code -- which is what the build is for.
"""

from __future__ import annotations

import html
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

XML_DIR = pathlib.Path("build/api-docs/xml")


def inline(elem: ET.Element | None) -> str:
    """Renders a doxygen description subtree as inline markdown."""
    if elem is None:
        return ""
    parts: list[str] = []
    _walk(elem, parts)
    text = "".join(parts)
    # Collapse the whitespace doxygen leaves around converted tags, but keep
    # paragraph breaks produced by _walk's sentinel.
    text = html.unescape(text)
    lines = [ln.rstrip() for ln in text.split("\n")]
    out: list[str] = []
    blank = False
    for ln in lines:
        if not ln.strip():
            if not blank and out:
                out.append("")
            blank = True
            continue
        blank = False
        out.append(ln)
    return "\n".join(out).strip()


def _children(elem: ET.Element, parts: list[str]) -> None:
    if elem.text:
        parts.append(elem.text)
    for child in elem:
        _walk(child, parts)


def _walk(elem: ET.Element, parts: list[str]) -> None:
    tag = elem.tag
    if tag == "para":
        parts.append("\n\n")
        _children(elem, parts)
        parts.append("\n\n")
    elif tag in ("bold", "emphasis"):
        parts.append("**")
        _children(elem, parts)
        parts.append("**")
    elif tag == "computeroutput":
        parts.append("`")
        _children(elem, parts)
        parts.append("`")
    elif tag == "programlisting":
        parts.append("\n\n```cpp\n")
        _children(elem, parts)
        parts.append("\n```\n\n")
    elif tag == "ulink":
        parts.append("[")
        _children(elem, parts)
        parts.append("](")
        parts.append(elem.get("url", ""))
        parts.append(")")
    elif tag == "ref":
        _children(elem, parts)
    elif tag == "itemizedlist":
        parts.append("\n\n")
        for item in elem:
            _walk(item, parts)
        parts.append("\n\n")
    elif tag == "listitem":
        parts.append("- ")
        _children(elem, parts)
        parts.append("\n")
    elif tag == "simplesect":
        kind = elem.get("kind", "note")
        parts.append(f"**{kind.capitalize()}:** ")
        _children(elem, parts)
        parts.append("\n\n")
    elif tag == "heading":
        parts.append("\n\n#### ")
        _children(elem, parts)
        parts.append("\n\n")
    elif tag == "linebreak":
        parts.append("  \n")
    elif tag == "sp":
        # Doxygen's highlighted program listings encode spaces as an empty
        # <sp/> element, which itertext() skips -- that is how
        # "avbase::Playerplayer;" happened.
        parts.append(" ")
    elif tag == "ndash":
        # `--` in a comment becomes an en dash element; itertext() drops it.
        parts.append("–")
    elif tag == "mdash":
        parts.append("—")
    elif tag == "hr":
        parts.append("\n\n---\n\n")
    elif tag == "blockquote":
        parts.append("\n\n> ")
        _children(elem, parts)
        parts.append("\n\n")
    elif tag == "table":
        for row in elem:
            if row.tag != "row":
                continue
            cells = [_text(c).strip() for c in row if c.tag == "entry"]
            parts.append("- " + " | ".join(cells) + "\n")
    else:
        _children(elem, parts)
    # The tail is text that FOLLOWS this element inside its parent; dropping
    # it silently eats the prose between two refs, which is how
    # "avbase_source_release()avbase_player_set_data_source()" happened.
    if elem.tail:
        parts.append(elem.tail)


def _text(elem: ET.Element) -> str:
    return "".join(elem.itertext())


def type_str(member: ET.Element) -> str:
    t = member.find("type")
    if t is None:
        return ""
    text = _text(t).strip()
    # Doxygen pads inside template angle brackets (`scoped_refptr< T >`); the
    # headers do not. Normalizing here keeps signatures copy-pasteable.
    text = re.sub(r"<\s+", "<", text)
    text = re.sub(r"\s+>", ">", text)
    return text


def signature(member: ET.Element) -> str:
    kind = member.get("kind")
    name = member.find("name").text or ""
    args = member.find("argsstring")
    args_s = args.text or "" if args is not None else ""
    if kind == "define":
        init = member.find("initializer")
        init_s = _text(init).strip() if init is not None else ""
        return f"#define {name} {init_s}".rstrip()
    if kind == "function":
        qual = ""
        if member.get("static") == "yes":
            qual += "static "
        if member.get("explicit") == "yes":
            qual += "explicit "
        ret = type_str(member)
        ret = (ret + " ") if ret else ""
        virt = member.get("virt", "non-virtual")
        # argsstring already carries the trailing `= 0` for pure virtuals;
        # appending it again is how `Post(...)=0 = 0` happened.
        return f"{qual}{ret}{name}{args_s}".strip()
    if kind == "typedef":
        return f"using {name} = {type_str(member)};"
    if kind == "enum":
        strong = member.get("strong") == "yes"
        return f"enum {'class ' if strong else ''}{name}"
    # variable
    init = member.find("initializer")
    init_s = ""
    if init is not None and init.text:
        init_s = " " + _text(init).strip()
    return f"{type_str(member)} {name}{init_s}".strip()


def description(member: ET.Element) -> str:
    brief = inline(member.find("briefdescription"))
    detailed = inline(member.find("detaileddescription"))
    if brief and detailed and detailed.startswith(brief):
        return detailed
    if brief and detailed:
        return brief + "\n\n" + detailed
    return detailed or brief


def enum_values(member: ET.Element) -> list[tuple[str, str, str]]:
    out = []
    for ev in member.findall("enumvalue"):
        name = ev.find("name").text or ""
        init_el = ev.find("initializer")
        init = _text(init_el).strip() if init_el is not None else ""
        doc = inline(ev.find("briefdescription")) or inline(
            ev.find("detaileddescription"))
        out.append((name, init, doc))
    return out


def member_sort_key(member: ET.Element) -> tuple[int, str]:
    order = {"enum": 0, "typedef": 1, "function": 2, "variable": 3}
    name = member.find("name")
    return (order.get(member.get("kind"), 9), name.text or "" if name is not None else "")


def collect_members(compound: ET.Element) -> list[ET.Element]:
    """Public memberdefs of a compound, in XML (source) order."""
    out = []
    for section in compound.findall("sectiondef"):
        for member in section.findall("memberdef"):
            if member.get("prot") == "public" and member.get("kind") != "friend":
                out.append(member)
    # File/namespace compounds put members directly under compounddef.
    for member in compound.findall("memberdef"):
        if member.get("prot") == "public" and member.get("kind") != "friend":
            out.append(member)
    return out


def header_of(compound: ET.Element, member: ET.Element) -> str:
    loc = member.find("location")
    if loc is not None and loc.get("file"):
        return loc.get("file")
    inc = compound.find("includes")
    if inc is not None and inc.text:
        return inc.text
    return "?"


def render_class(compound: ET.Element) -> list[str]:
    name = compound.find("compoundname").text or ""
    kind = compound.get("kind")
    loc = compound.find("location")
    where = f"{loc.get('file')}:{loc.get('line')}" if loc is not None else ""
    doc = inline(compound.find("briefdescription"))
    detailed = inline(compound.find("detaileddescription"))
    head = f"### `{kind}` {name}"
    out = [head, ""]
    if where:
        out.append(f"*{where}*")
        out.append("")
    body = doc or detailed
    if doc and detailed and not detailed.startswith(doc):
        body = doc + "\n\n" + detailed
    if body:
        out.append(body)
        out.append("")
    for member in collect_members(compound):
        mdoc = description(member)
        if member.get("kind") == "enum":
            out.append(f"- `{signature(member)}`")
            if mdoc:
                out.append(f"  {mdoc.splitlines()[0]}")
            for vname, vinit, vdoc in enum_values(member):
                init = f" `{vinit}`" if vinit else ""
                out.append(f"  - `{vname}`{init}" + (f" — {vdoc}" if vdoc else ""))
        else:
            out.append(f"- `{signature(member)}`")
            if mdoc:
                for ln in mdoc.splitlines():
                    out.append(f"  {ln}" if ln else "")
    out.append("")
    return out


def render_file_member(member: ET.Element) -> list[str]:
    mdoc = description(member)
    out = []
    if member.get("kind") == "enum":
        out.append(f"- `{signature(member)}`")
        if mdoc:
            out.append(f"  {mdoc.splitlines()[0]}")
        for vname, vinit, vdoc in enum_values(member):
            init = f" `{vinit}`" if vinit else ""
            out.append(f"  - `{vname}`{init}" + (f" — {vdoc}" if vdoc else ""))
    else:
        out.append(f"- `{signature(member)}`")
        if mdoc:
            for ln in mdoc.splitlines():
                out.append(f"  {ln}" if ln else "")
    return out


def main() -> int:
    if not XML_DIR.is_dir():
        print(f"no doxygen XML at {XML_DIR}; run tools/gen_api_docs.sh first",
              file=sys.stderr)
        return 2
    # header path -> rendered blocks. Classes are contributed by their own
    # compound XMLs and file/namespace-scope members by theirs; both key on
    # the member's <location file=...>, so one header collects everything
    # declared in it. Class compounds sort before file compounds (class* <
    # *_8h), so classes render first within a header.
    sections: dict[str, list[str]] = {}
    seen: set[str] = set()
    for path in sorted(XML_DIR.glob("*.xml")):
        if path.name in ("index.xml", "Doxyfile.xml"):
            continue
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError as exc:
            print(f"XML parse error in {path}: {exc}", file=sys.stderr)
            return 2
        for compound in root.findall("compounddef"):
            kind = compound.get("kind")
            if kind in ("class", "struct"):
                loc = compound.find("location")
                header = loc.get("file") if loc is not None else "?"
                sections.setdefault(header, []).extend(render_class(compound))
            elif kind in ("file", "namespace"):
                for member in collect_members(compound):
                    mid = member.get("id")
                    if mid in seen:
                        continue
                    seen.add(mid)
                    loc = member.find("location")
                    header = (loc.get("file") if loc is not None
                              and loc.get("file") else "?")
                    sections.setdefault(header, []).extend(
                        render_file_member(member))
    out: list[str] = []
    out.append("# avbase API 参考")
    out.append("")
    out.append("> **本文件由工具生成，勿手改。** 管线：`tools/gen_api_docs.sh` →")
    out.append("> Doxygen（`docs/Doxyfile`，XML 后端，INPUT_FILTER 把 public 头的")
    out.append("> `//` 注释转成 doxygen 语法）→ `tools/doxy_xml_to_md.py`。")
    out.append("> 验收标准：Doxygen 运行**零 warning**（docs/10 §12）。改注释请改")
    out.append("> `player/public/` 下的头文件后重新生成。")
    out.append("")
    out.append("## 目录")
    out.append("")
    for header in sorted(sections):
        out.append(f"- {header}")
    out.append("")
    for header in sorted(sections):
        blocks = sections[header]
        if not blocks:
            continue
        out.append(f"## {header}")
        out.append("")
        out.extend(blocks)
    sys.stdout.write("\n".join(out).rstrip() + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
