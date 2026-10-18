#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Golden 对拍（docs/07 §7）。对比两份 JSONL 录制（baseline vs actual），按
# docs/07 §7.4 的容差分级判定 pass/fail，有意差异由 allowlist.yaml 显式登记。
#
# ── 诚实边界（读这个工具之前先看这里）────────────────────────────────
#   * 这是 golden 基建的 **diff 件**，不是录制件。0.1.0 没有可对比的基线：
#     录制基线要 ijkplayer 原版（docs/07 §7.3），而它被开放问题 Q8
#     （golden 基线锁哪个 ijkplayer 版本，docs/08 §7 Q8）卡住，且本沙箱既
#     无原版也拿不到 GitHub tarball。所以 CI 不跑对拍（没有基线可对比，
#     跑出来等于没测）；本工具等基线就位即可用，且自带 --selftest 证明
#     判定逻辑本身是对的。
#   * audio.checksum 按**严格相等**判定，不是 §7.4 写的 "逐样本差 ≤ 1 LSB"。
#     原因是录制格式（§7.2）里 checksum 是 PCM 的 CRC32 —— 一个 32 bit 摘要
#     无法表达"差 1 LSB"，逐样本容差需要 samples 数组，而格式里没有。这里
#     如实按全等比较，需要豁免时走 allowlist（§7.6 的例子正是 checksum）。
#   * 零外部依赖：不 import yaml。tolerance/allowlist 用极简 YAML 子集解析
#     （扁平 `key: value` 与 `- key: value` 列表项），与 V9/V10 的
#     "CI runner 不必装东西" 口径一致。
#
# Usage:
#   python3 tools/golden_diff.py BASELINE.jsonl ACTUAL.jsonl
#   python3 tools/golden_diff.py B.jsonl A.jsonl --tolerance golden/tolerance.yaml \
#       --allowlist golden/allowlist.yaml --case g01_basic_mp4
#   python3 tools/golden_diff.py --selftest

import argparse
import json
import sys
import tempfile
from pathlib import Path

from _jsonl import load_jsonl

# docs/07 §7.4 容差分级表。0 = 必须完全一致；正数是绝对容差（除 event.arg
# 是相对比例）。'skip' = 不做 golden（阶段耗时走性能基准）。
DEFAULT_TOLERANCE = {
    "video.count": 0,          # framedrop=0 时帧数必须一致
    "video.pts": 0,
    "video.dts": 0,
    "video.hash_hamming": 5,   # 感知哈希汉明距离，容忍 sws 舍入
    "video.late_us": 5000,     # 调度时机受系统影响
    "video.dropped": 0,
    "audio.count": 0,
    "audio.pts": 0,
    "audio.checksum": 0,       # CRC32 只能全等，见文件头说明
    "audio.nb_samples": 0,
    "event.sequence": 0,       # 类型与顺序完全一致
    "event.arg": 0.05,         # arg 值允许 ±5%
    "event.at_ms": 5000,       # 调度时机（外推自 late_us 的精神）
    "state.sequence": 0,
    "state.at_ms": 5000,
    "stat": "skip",
}


def _parse_scalar(v):
    s = v.strip()
    if s in ("", "null", "~"):
        return None
    if s.lower() in ("true", "false"):
        return s.lower() == "true"
    try:
        return int(s)
    except ValueError:
        pass
    try:
        return float(s)
    except ValueError:
        pass
    return s.strip('"').strip("'")


def load_tolerance(path):
    """极简 YAML 子集：扁平 `key: value`，# 开头整行注释。"""
    out = {}
    if path is None:
        return out
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or ":" not in line:
            continue
        k, v = line.split(":", 1)
        out[k.strip()] = _parse_scalar(v)
    return out


def load_allowlist(path):
    """极简 YAML 子集：`- key: value` 列表项，后续 `key: value` 归入当前项。"""
    items = []
    if path is None:
        return items
    cur = None
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("- "):
            if cur is not None:
                items.append(cur)
            cur = {}
            line = line[2:].strip()
            if not line:
                continue
        if ":" not in line:
            continue
        k, v = line.split(":", 1)
        if cur is None:
            cur = {}
        cur[k.strip()] = _parse_scalar(v)
    if cur is not None:
        items.append(cur)
    return items


def hamming_hex(a, b):
    """感知哈希（§7.2 的 64 bit DCT 低频哈希）的汉明距离。"""
    try:
        return bin(int(str(a), 16) ^ int(str(b), 16)).count("1")
    except (ValueError, TypeError):
        return None


def _allowed(allow, case, field):
    """返回 (bool, reason)：该 case 的该 field 是否被 allowlist 豁免。"""
    for item in allow:
        if item.get("case") in (case, "*", None) and \
                item.get("field") in (field, "*"):
            return True, item.get("reason", "")
    return False, ""


def _finding(field, kind, expected, actual, allowed, reason=""):
    return {"field": field, "kind": kind, "expected": expected,
            "actual": actual, "allowed": allowed, "reason": reason}


def _cmp_num(findings, allow, case, field, key, b, a, tol):
    """数值比较：tol 为绝对容差；0 表示必须相等。"""
    if key not in b and key not in a:
        return
    if key not in b or key not in a:
        allowed, reason = _allowed(allow, case, field)
        findings.append(_finding(field, "missing", b.get(key), a.get(key),
                                 allowed, reason))
        return
    bv, av = b.get(key), a.get(key)
    if not isinstance(bv, (int, float)) or not isinstance(av, (int, float)):
        if bv != av:
            allowed, reason = _allowed(allow, case, field)
            findings.append(_finding(field, "mismatch", bv, av, allowed, reason))
        return
    if abs(bv - av) > tol:
        allowed, reason = _allowed(allow, case, field)
        findings.append(_finding(field, "tolerance", bv, av, allowed, reason))


def compare_video(base, act, tol, allow, case):
    findings = []
    cnt_tol = tol.get("video.count", 0)
    if abs(len(base) - len(act)) > cnt_tol:
        allowed, reason = _allowed(allow, case, "video.count")
        findings.append(_finding("video.count", "count", len(base), len(act),
                                 allowed, reason))
        return findings
    for i, (b, a) in enumerate(zip(base, act)):
        for key in ("pts", "dts"):
            _cmp_num(findings, allow, case, f"video.{key}", key, b, a,
                     tol.get(f"video.{key}", 0))
        _cmp_num(findings, allow, case, "video.late_us", "late_us", b, a,
                 tol.get("video.late_us", 5000))
        if b.get("dropped") != a.get("dropped"):
            allowed, reason = _allowed(allow, case, "video.dropped")
            findings.append(_finding("video.dropped", "mismatch",
                                     b.get("dropped"), a.get("dropped"),
                                     allowed, reason))
        if "hash" in b and "hash" in a:
            d = hamming_hex(b["hash"], a["hash"])
            limit = tol.get("video.hash_hamming", 5)
            if d is None or d > limit:
                allowed, reason = _allowed(allow, case, "video.hash")
                findings.append(_finding("video.hash",
                                         "unhashable" if d is None else "dct",
                                         f"distance<={limit}", f"distance={d}",
                                         allowed, reason))
    return findings


def compare_audio(base, act, tol, allow, case):
    findings = []
    if abs(len(base) - len(act)) > tol.get("audio.count", 0):
        allowed, reason = _allowed(allow, case, "audio.count")
        findings.append(_finding("audio.count", "count", len(base), len(act),
                                 allowed, reason))
        return findings
    for b, a in zip(base, act):
        _cmp_num(findings, allow, case, "audio.pts", "pts", b, a,
                 tol.get("audio.pts", 0))
        _cmp_num(findings, allow, case, "audio.nb_samples", "nb_samples", b, a,
                 tol.get("audio.nb_samples", 0))
        if "checksum" in b or "checksum" in a:
            if b.get("checksum") != a.get("checksum"):
                allowed, reason = _allowed(allow, case, "audio.checksum")
                findings.append(_finding("audio.checksum", "crc32",
                                         b.get("checksum"), a.get("checksum"),
                                         allowed, reason))
    return findings


def compare_events(base, act, tol, allow, case):
    findings = []
    seqb = [(e.get("msg"), e.get("name")) for e in base]
    seqa = [(e.get("msg"), e.get("name")) for e in act]
    if seqb != seqa:
        allowed, reason = _allowed(allow, case, "event.sequence")
        findings.append(_finding("event.sequence", "order", seqb, seqa,
                                 allowed, reason))
        return findings
    rel = tol.get("event.arg", 0.05)
    for i, (b, a) in enumerate(zip(base, act)):
        for key in ("arg1", "arg2"):
            if key not in b or key not in a:
                continue
            bv, av = b[key], a[key]
            if not isinstance(bv, (int, float)) or not isinstance(av, (int, float)):
                if bv != av:
                    allowed, reason = _allowed(allow, case, f"event.{key}")
                    findings.append(_finding(f"event.{key}", "mismatch", bv, av,
                                             allowed, reason))
                continue
            denom = abs(bv) if bv else 1.0
            if abs(bv - av) / denom > rel:
                allowed, reason = _allowed(allow, case, f"event.{key}")
                findings.append(_finding(f"event.{key}", "relative", bv, av,
                                         allowed, reason))
        _cmp_num(findings, allow, case, "event.at_ms", "at_ms", b, a,
                 tol.get("event.at_ms", 5000))
    return findings


def compare_states(base, act, tol, allow, case):
    findings = []
    seqb = [(s.get("from"), s.get("to")) for s in base]
    seqa = [(s.get("from"), s.get("to")) for s in act]
    if seqb != seqa:
        allowed, reason = _allowed(allow, case, "state.sequence")
        findings.append(_finding("state.sequence", "order", seqb, seqa,
                                 allowed, reason))
        return findings
    for b, a in zip(base, act):
        _cmp_num(findings, allow, case, "state.at_ms", "at_ms", b, a,
                 tol.get("state.at_ms", 5000))
    return findings


def run_diff(baseline, actual, tolerance=None, allowlist=None, case="",
             out=sys.stdout):
    tol = dict(DEFAULT_TOLERANCE)
    tol.update(load_tolerance(tolerance))
    allow = load_allowlist(allowlist)
    brows = load_jsonl(baseline)
    arows = load_jsonl(actual)

    def pick(rows, t):
        return [r for r in rows if r.get("t") == t]

    findings = []
    findings += compare_video(pick(brows, "video"), pick(arows, "video"),
                              tol, allow, case)
    findings += compare_audio(pick(brows, "audio"), pick(arows, "audio"),
                              tol, allow, case)
    findings += compare_events(pick(brows, "event"), pick(arows, "event"),
                               tol, allow, case)
    findings += compare_states(pick(brows, "state"), pick(arows, "state"),
                               tol, allow, case)

    print(f"golden diff: {baseline} vs {actual}", file=out)
    print(f"  records: baseline={len(brows)} actual={len(arows)}", file=out)
    stat_n = len(pick(brows, "stat")) + len(pick(arows, "stat"))
    if stat_n:
        print(f"  stat records: {stat_n} (skipped: docs/07 §7.4 — 阶段耗时走"
              f"性能基准，不做 golden)", file=out)

    blocking = 0
    for f in findings:
        tag = "ALLOWED" if f["allowed"] else "FAIL"
        if not f["allowed"]:
            blocking += 1
        reason = f"  ({f['reason']})" if f["reason"] else ""
        print(f"  [{tag}] {f['field']} ({f['kind']}): "
              f"expected={f['expected']} actual={f['actual']}{reason}", file=out)

    if not findings:
        print("  OK: no differences", file=out)
    print(f"  findings={len(findings)} blocking={blocking}", file=out)
    return blocking


def selftest():
    """验证判定逻辑本身（不依赖任何录制基线）。"""
    import io

    def run(brows, arows, allow=None, case="t"):
        with tempfile.TemporaryDirectory() as d:
            bp = Path(d) / "b.jsonl"
            ap = Path(d) / "a.jsonl"
            bp.write_text("\n".join(json.dumps(r) for r in brows) + "\n")
            ap.write_text("\n".join(json.dumps(r) for r in arows) + "\n")
            alp = None
            if allow:
                alp = Path(d) / "allow.yaml"
                alp.write_text("\n".join(allow) + "\n")
            buf = io.StringIO()
            return run_diff(bp, ap, allowlist=alp, case=case, out=buf)

    failures = []

    # 1) 完全一致 → 0 blocking
    same = [{"t": "video", "n": 0, "pts": 0, "hash": "0000000000000000",
             "late_us": 0}]
    if run(same, same) != 0:
        failures.append("identical recordings must produce no blocking finding")

    # 2) pts 差 1µs → 必须 fail（±0）
    b = [{"t": "video", "n": 0, "pts": 0, "hash": "0000000000000000"}]
    a = [{"t": "video", "n": 0, "pts": 1, "hash": "0000000000000000"}]
    if run(b, a) != 1:
        failures.append("video.pts ±0: a 1us drift must block")

    # 3) 感知哈希：汉明 4 通过，汉明 6 阻断
    bh = [{"t": "video", "n": 0, "pts": 0, "hash": "0000000000000000"}]
    a4 = [{"t": "video", "n": 0, "pts": 0, "hash": "000000000000000f"}]
    a6 = [{"t": "video", "n": 0, "pts": 0, "hash": "000000000000003f"}]
    if run(bh, a4) != 0:
        failures.append("hash hamming 4 must pass (tolerance 5)")
    if run(bh, a6) != 1:
        failures.append("hash hamming 6 must block (tolerance 5)")

    # 4) late_us：4000 通过，6000 阻断
    bl = [{"t": "video", "n": 0, "pts": 0, "late_us": 0}]
    if run(bl, [{"t": "video", "n": 0, "pts": 0, "late_us": 4000}]) != 0:
        failures.append("late_us 4000 must pass (tolerance 5000)")
    if run(bl, [{"t": "video", "n": 0, "pts": 0, "late_us": 6000}]) != 1:
        failures.append("late_us 6000 must block (tolerance 5000)")

    # 5) 事件顺序颠倒 → 阻断
    be = [{"t": "event", "msg": 200, "name": "Prepared"},
          {"t": "event", "msg": 400, "name": "VideoSizeChanged"}]
    ae = [{"t": "event", "msg": 400, "name": "VideoSizeChanged"},
          {"t": "event", "msg": 200, "name": "Prepared"}]
    if run(be, ae) != 1:
        failures.append("event sequence order must block")

    # 6) audio.checksum 不同 → 阻断；allowlist 登记后 → 豁免
    ba = [{"t": "audio", "n": 0, "pts": 0, "checksum": "9e2b1f00"}]
    aa = [{"t": "audio", "n": 0, "pts": 0, "checksum": "9e2b1f01"}]
    if run(ba, aa) != 1:
        failures.append("audio checksum mismatch must block")
    allow = ["- case: g11_speed_2x",
             "  field: audio.checksum",
             "  tolerance: full",
             '  reason: "Δ17 — 变速算法换成 WSOLA，样本级必然不同"']
    if run(ba, aa, allow=allow, case="g11_speed_2x") != 0:
        failures.append("allowlisted audio.checksum must be exempt")

    # 7) stat 不做 golden
    bs = [{"t": "stat", "video_decode_fps": 29.97}]
    as_ = [{"t": "stat", "video_decode_fps": 12.34}]
    if run(bs, as_) != 0:
        failures.append("stat records must be skipped (docs/07 §7.4)")

    if failures:
        for f in failures:
            print(f"selftest FAIL: {f}")
        return 1
    print("selftest OK")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("baseline", nargs="?", help="baseline JSONL recording")
    p.add_argument("actual", nargs="?", help="actual JSONL recording")
    p.add_argument("--tolerance", help="tolerance.yaml (docs/07 §7.4)")
    p.add_argument("--allowlist", help="allowlist.yaml (docs/07 §7.6)")
    p.add_argument("--case", default="", help="golden case name (allowlist key)")
    p.add_argument("--selftest", action="store_true", help="run built-in checks")
    args = p.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.baseline or not args.actual:
        print("error: BASELINE and ACTUAL are required (or --selftest)",
              file=sys.stderr)
        return 2
    blocking = run_diff(args.baseline, args.actual, args.tolerance,
                        args.allowlist, args.case)
    return 1 if blocking else 0


if __name__ == "__main__":
    raise SystemExit(main())
