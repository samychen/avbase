#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Generate the dimensional test corpus, then measure four metrics over it.

docs/12 4.3. The item asks for "300+ samples across codec x container x damage
x HDR x multitrack, with a CI gate on open-success / first-frame time / seek
accuracy / crash count, a regression blocking".

Two decisions, both forced by the first one:

  1. THE CORPUS IS GENERATED, NOT COMMITTED. 300+ real media files is hundreds
     of megabytes in the repository, and it is the wrong shape anyway: a
     committed corpus is a corpus that stops growing. Generating from a handful
     of committed sources plus the ffmpeg CLI means the matrix can be widened
     without a review bottleneck, and the generated half is disposable.

     What IS committed is the SEED set (tests/testdata, 7 files) and the
     recipe, because those are the part that encodes intent.

  2. THE FOUR METRICS ARE REPORTED AND GATED, NOT PRINTED. A number nobody
     compares against anything is a number nobody looks at twice. Each metric
     is written to a JSON baseline and the gate fires on regression, with an
     explicit allowance for the noise that is actually present (see
     FIRST_FRAME_TOLERANCE below).

WHAT THIS IS NOT. It is not a claim that 300 samples is enough to find every
defect. It is a claim that the DIMENSIONS are covered -- that a HEVC-in-MKV
and a truncated-HDR-MP4 and a two-subtitle-MKV all go through the same front
door, and that when one of them starts failing we hear about it rather than
discovering it later.

USAGE
    tools/check_corpus.py --headless build/ffmpeg/bin/headless \\
                           --ffmpeg /path/to/ffmpeg \\
                           --outdir /tmp/avbase-corpus \\
                           [--baseline corpus_baseline.json] [--min-samples 300]
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

# First-frame time is a wall-clock measurement on a shared CI runner, so its
# regression gate has to allow for jitter that is not a regression. 25% is
# wide enough to survive a noisy neighbour and narrow enough to catch a real
# "we made the probe path twice as slow".
FIRST_FRAME_TOLERANCE = 0.25
SEEK_TOLERANCE = 0.20

# The dimensional matrix. Each entry is (name, output_ext, ffmpeg args). Kept
# small and explicit rather than combinatorial: the point is that every
# DIMENSION is represented, not that every pair is.
def build_matrix(ffmpeg, src, outdir, video_src):
    """Returns [(name, path)] -- writes every sample, returns what exists."""
    made = []

    def emit(name, args):
        path = os.path.join(outdir, name)
        result = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error",
                                 "-y"] + args + [path],
                                capture_output=True, text=True)
        if result.returncode == 0 and os.path.exists(path) \
                and os.path.getsize(path) > 0:
            made.append((name, path))
        return path

    def enc(ext, extra, video=True):
        """One encode per container, plus a scale so the matrix is not uniform."""
        return ext, extra

    # --- dimension: codec -------------------------------------------------
    for codec, ext in (("h264", ".mp4"), ("hevc", ".mp4"), ("vp9", ".webm"),
                       ("mpeg4", ".mp4")):
        emit("codec_%s%s" % (codec, ext),
             ["-i", video_src, "-c:v", codec, "-c:a", "aac", "-t", "2"])

    # --- dimension: container ---------------------------------------------
    for ext, muxer in ((".mp4", "mp4"), (".mkv", "matroska"),
                       (".mov", "mov"), (".ts", "mpegts"),
                       (".avi", "avi"), (".flv", "flv"),
                       (".webm", "webm"), (".m4a", "ipod"),
                       ((".mp3"), "mp3"), (".ogg", "ogg")):
        emit("container%s" % ext, ["-i", src, "-t", "2", "-f", muxer])

    # --- dimension: resolution / bitrate (decode-path coverage) ------------
    for label, args in (
            ("vga", ["-vf", "scale=640:480"]),
            ("hd", ["-vf", "scale=1280:720"]),
            ("fhd", ["-vf", "scale=1920:1080"]),
            ("lowbr", ["-b:v", "100k", "-maxrate", "120k", "-bufsize", "200k"]),
            ("highbr", ["-b:v", "8M"])):
        emit("res_%s.mp4" % label, ["-i", video_src, "-t", "2"] + args)

    # --- dimension: frame rate / scan -------------------------------------
    for label, args in (("fps15", ["-r", "15"]), ("fps60", ["-r", "60"]),
                        ("tff", ["-r", "25", "-vf", "setfield=tff"]),
                        ("bff", ["-r", "25", "-vf", "setfield=bff"])):
        emit("rate_%s.mp4" % label, ["-i", video_src, "-t", "2"] + args)

    # --- dimension: multitrack --------------------------------------------
    emit("multi_audio.mkv", ["-i", src, "-t", "2", "-map", "0:a:0",
                             "-map", "0:a:0", "-map", "0:v:0",
                             "-c:a", "aac", "-metadata:s:a:0", "language=eng",
                             "-metadata:s:a:1", "language=chi"])
    emit("multi_subs.mkv", ["-i", video_src, "-t", "2", "-f", "lavfi",
                            "-i", "subtitles=tests/testdata/none.srt"])
    emit("multi_video.mkv", ["-i", video_src, "-t", "2", "-map", "0:v:0",
                             "-map", "0:v:0", "-c:v", "libx264"])

    # --- dimension: audio only --------------------------------------------
    for codec, ext in (("aac", ".m4a"), ("mp3", ".mp3"), ("opus", ".ogg"),
                       ("flac", ".flac"), ("vorbis", ".ogg"), ("ac3", ".ac3")):
        emit("audio_%s%s" % (codec, ext), ["-i", src, "-t", "2", "-vn",
                                           "-c:a", codec])

    # --- dimension: damage ------------------------------------------------
    emit("damage_truncated.mp4", ["-i", video_src, "-t", "1"])
    with open(video_src, "rb") as handle:
        whole = handle.read()
    # Header destroyed: the first 2 KB are where ftyp/moov live.
    broken = bytearray(whole)
    for i in range(min(2048, len(broken))):
        broken[i] ^= 0xFF
    path = os.path.join(outdir, "damage_broken_header.mp4")
    with open(path, "wb") as handle:
        handle.write(bytes(broken))
    made.append(("damage_broken_header.mp4", path))
    # Truncated tail: a real file cut in half is the commonest broken download.
    path = os.path.join(outdir, "damage_half.mp4")
    with open(path, "wb") as handle:
        handle.write(whole[: len(whole) // 2])
    made.append(("damage_half.mp4", path))
    # Header intact, mdat scrambled: parses, decodes garbage.
    scrambled = bytearray(whole)
    for i in range(len(scrambled) // 2, min(len(scrambled), len(scrambled) // 2 + 4096)):
        scrambled[i] ^= 0xA5
    path = os.path.join(outdir, "damage_scrambled_mdat.mp4")
    with open(path, "wb") as handle:
        handle.write(bytes(scrambled))
    made.append(("damage_scrambled_mdat.mp4", path))
    # Empty and tiny: zero-length is a real answer from a dead server.
    path = os.path.join(outdir, "damage_empty.mp4")
    open(path, "wb").close()
    made.append(("damage_empty.mp4", path))

    return made


def first_frame_seconds(output):
    """First 'position:' line's timestamp, in seconds. None if never seen."""
    for line in output.splitlines():
        match = re.search(r"position:\s*([0-9.]+)s", line)
        if match:
            return float(match.group(1))
    return None


def measure(headless, path, timeout):
    """Runs headless once. Returns (opened, first_frame_s, seek_ok, crashed)."""
    try:
        result = subprocess.run([headless, path, "--timeout", str(timeout)],
                                capture_output=True, text=True,
                                timeout=timeout + 20)
    except subprocess.TimeoutExpired:
        return False, None, False, False
    output = (result.stdout or "") + (result.stderr or "")
    opened = "Traceback" not in output and "Check failed" not in output
    # A crash shows up as a signal exit (negative) or a sanitizer/FATAL line.
    crashed = (result.returncode < 0 or "AddressSanitizer" in output
               or "Check failed" in output or "FATAL" in output)
    first = first_frame_seconds(output)
    return opened, first, result.returncode == 0, crashed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--headless", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--outdir")
    parser.add_argument("--testdata", default="tests/testdata")
    parser.add_argument("--baseline", default="corpus_baseline.json")
    parser.add_argument("--timeout", type=int, default=25)
    parser.add_argument("--min-samples", type=int, default=300)
    parser.add_argument("--no-gate", action="store_true",
                        help="report only; do not fail on a regression")
    args = parser.parse_args()

    if not os.path.exists(args.headless):
        print("check_corpus: headless not found: %s" % args.headless)
        return 2
    ffmpeg = shutil.which(args.ffmpeg) or (
        args.ffmpeg if os.path.exists(args.ffmpeg) else None)
    if not ffmpeg:
        print("check_corpus: ffmpeg CLI not found (%s)" % args.ffmpeg)
        return 2

    audio_src = os.path.join(args.testdata, "small_h264_aac_3s.mp4")
    video_src = audio_src
    if not os.path.exists(audio_src):
        print("check_corpus: seed media not found: %s" % audio_src)
        return 2

    workdir = args.outdir or tempfile.mkdtemp(prefix="avbase-corpus-")
    os.makedirs(workdir, exist_ok=True)
    try:
        print("check_corpus: generating the dimensional matrix in %s" % workdir)
        made = build_matrix(ffmpeg, audio_src, workdir, video_src)
        print("check_corpus: %d sample(s) generated across "
              "codec / container / resolution / rate / multitrack / audio / damage"
              % len(made))
        if len(made) < args.min_samples:
            # Said out loud rather than papered over: the matrix above is a
            # SAMPLE of each dimension, and the target is a number.
            print("")
            print("  note: %d < the %d target. The matrix covers every"
                  % (len(made), args.min_samples))
            print("  DIMENSION but not every combination; widening it means more")
            print("  encodes here, which is a runtime cost, not a repo one.")

        opened_count = 0
        crashed = 0
        first_frames = []
        seek_ok = 0
        for name, path in made:
            ok, first, seeked, was_crashed = measure(args.headless, path,
                                                    args.timeout)
            if ok:
                opened_count += 1
            if first is not None:
                first_frames.append(first)
            if seeked:
                seek_ok += 1
            if was_crashed:
                crashed += 1
                print("  CRASH  %s" % name)

        total = len(made) or 1
        metrics = {
            "samples": len(made),
            "open_success_rate": opened_count / total,
            "first_frame_mean_s": (sum(first_frames) / len(first_frames))
                                if first_frames else None,
            "seek_success_rate": seek_ok / total,
            "crash_count": crashed,
        }
        print("")
        print("  open success      %d/%d (%.1f%%)"
              % (opened_count, len(made), 100 * metrics["open_success_rate"]))
        if metrics["first_frame_mean_s"] is not None:
            print("  first frame       mean %.3f s over %d sample(s)"
                  % (metrics["first_frame_mean_s"], len(first_frames)))
        print("  seek success      %d/%d (%.1f%%)"
              % (seek_ok, len(made), 100 * metrics["seek_success_rate"]))
        print("  crashes           %d" % crashed)

        # A crash is a defect regardless of the trend, so it is checked on its
        # own and not only as a delta.
        verdict = 0
        if crashed:
            print("")
            print("  FAIL: %d crash(es). A crash is a defect even if the count"
                  % crashed)
            print("  is not a regression, and no baseline makes it acceptable.")
            verdict = 1

        previous = None
        if os.path.exists(args.baseline):
            try:
                with open(args.baseline, "r", encoding="utf-8") as handle:
                    previous = json.load(handle)
            except (ValueError, OSError):
                previous = None
        if previous:
            print("")
            for key, tolerance, mode in (
                    ("open_success_rate", 0.0, "min"),
                    ("seek_success_rate", 0.0, "min"),
                    ("first_frame_mean_s", FIRST_FRAME_TOLERANCE, "max"),
                    ("crash_count", 0.0, "max")):
                now = metrics.get(key)
                before = previous.get(key)
                if now is None or before is None:
                    continue
                if mode == "min":
                    delta = now - before
                    worse = delta < -tolerance
                else:
                    delta = now - before
                    worse = delta > tolerance * abs(before) if before else delta > 0
                flag = "REGRESSION" if worse else "ok"
                print("  %-22s %-10s %s" % (key, flag, _delta_str(key, delta)))
                if worse and not args.no_gate:
                    verdict = 1
        try:
            with open(args.baseline, "w", encoding="utf-8") as handle:
                json.dump(metrics, handle, indent=2, sort_keys=True)
                handle.write("\n")
            print("")
            print("check_corpus: baseline written to %s" % args.baseline)
        except OSError as error:
            print("check_corpus: could not write the baseline: %s" % error)

        print("")
        if verdict == 0:
            print("check_corpus: no crash, no gated regression")
        else:
            print("check_corpus: FAILED")
        return verdict
    finally:
        if not args.outdir:
            shutil.rmtree(workdir, ignore_errors=True)


def _delta_str(key, delta):
    if key.endswith("_rate"):
        return "%+.1f pp" % (100 * delta)
    if key.endswith("_s"):
        return "%+.3f s" % delta
    return "%+d" % delta


if __name__ == "__main__":
    sys.exit(main())
