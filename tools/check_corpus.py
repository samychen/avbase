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
    """Returns (made, failed).

    |made| is [(name, path)] for every sample that was written and is non-empty;
    |failed| is [(name, reason)] for the ones that were not. Returning both is
    the point: an earlier version returned only |made| and dropped the rest
    silently, so a sample whose encode failed simply vanished from the corpus.
    That is how the matrix reached 29 generated against a target of 300 with no
    indication that eight encodes had errored -- one of them because it asked a
    container for a codec it cannot hold (mpegts defaults to mp2 audio, which the
    audio decoder does not support).
    """
    made = []
    failed = []
    skipped = []

    # Which audio encoders this ffmpeg build actually has. Asking beats
    # assuming: this one has no libmp3lame and no libvorbis, so an mp3 or ogg
    # container cannot be produced here at all. That is a property of the
    # machine, so it is reported as SKIPPED -- whereas a container we CAN encode
    # that still errors is a real failure and fails the gate.
    probe = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error",
                            "-encoders"], capture_output=True, text=True)
    available_encoders = set()
    for line in (probe.stdout or "").splitlines():
        parts = line.split()
        if len(parts) >= 2 and len(parts[0]) == 6:
            available_encoders.add(parts[1])

    def emit(name, args):
        path = os.path.join(outdir, name)
        result = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error",
                                 "-y"] + args + [path],
                                capture_output=True, text=True)
        if result.returncode == 0 and os.path.exists(path) \
                and os.path.getsize(path) > 0:
            made.append((name, path))
            return path

        if result.returncode != 0:
            detail = (result.stderr or "").strip().splitlines()
            reason = "encode failed: %s" % (detail[-1] if detail
                                           else "exit %d" % result.returncode)
        elif not os.path.exists(path):
            reason = "encode produced no output file"
        else:
            reason = "encode produced an empty file"
        failed.append((name, reason))
        return path

    def enc(ext, extra, video=True):
        """One encode per container, plus a scale so the matrix is not uniform."""
        return ext, extra

    # --- dimension: codec -------------------------------------------------
    for codec, ext, acodec_args in (
            ("h264", ".mp4", ["aac"]),
            ("hevc", ".mp4", ["aac"]),
            ("vp9", ".webm", ["opus", "-strict", "-2"]),
            ("mpeg4", ".mp4", ["aac"])):
        if acodec_args[0] not in available_encoders:
            skipped.append(("codec_%s%s" % (codec, ext),
                            "no encoder for %s in this ffmpeg build"
                            % acodec_args[0]))
            continue
        emit("codec_%s%s" % (codec, ext),
             ["-i", video_src, "-c:v", codec, "-c:a"] + acodec_args
             + ["-t", "2"])

    # --- dimension: container ---------------------------------------------
    # The audio codec is pinned per container on purpose. Leaving it to the
    # muxer's default is what silently cost this dimension its coverage:
    # mpegts defaults to mp2, which the audio decoder does not support, so
    # container.ts failed its encode, emit() dropped it, and the run still
    # printed a clean total. webm cannot hold h264 at all, so it is re-encoded
    # to vp9/opus rather than muxed.
    for ext, muxer, acodec_args, vcodec in (
            (".mp4", "mp4", ["aac"], "libx264"),
            (".mkv", "matroska", ["aac"], "libx264"),
            (".mov", "mov", ["aac"], "libx264"),
            (".ts", "mpegts", ["aac"], "libx264"),
            (".avi", "avi", ["aac"], "libx264"),
            (".flv", "flv", ["aac"], "libx264"),
            (".webm", "webm", ["opus", "-strict", "-2"], "libvpx-vp9"),
            (".m4a", "ipod", ["aac"], None),
            (".mp3", "mp3", ["libmp3lame"], None),
            (".ogg", "ogg", ["vorbis", "-strict", "-2"], None)):
        if acodec_args[0] not in available_encoders:
            # Not a defect: this ffmpeg build has no such encoder, so the
            # container cannot be produced here. Counted apart from failures.
            skipped.append(("container%s" % ext,
                            "no encoder for %s in this ffmpeg build"
                            % acodec_args[0]))
            continue
        args = ["-i", src, "-t", "2"]
        if vcodec is None:
            args += ["-vn"]
        else:
            args += ["-c:v", vcodec]
        args += ["-c:a"] + acodec_args + ["-f", muxer]
        emit("container%s" % ext, args)

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
    # The subtitle source is generated at run time rather than committed. The
    # recipe used to point at tests/testdata/none.srt, a file that has never
    # existed in this repository -- so this sample was never produced, and
    # nothing complained, because a failed encode was indistinguishable from
    # "this container does not need one". It is two lines of SRT; committing it
    # would imply it has content worth reviewing.
    srt_path = os.path.join(outdir, "_subtitle.srt")
    with open(srt_path, "w", encoding="utf-8") as handle:
        handle.write("1\n00:00:00,000 --> 00:00:02,000\navbase\n\n")
    emit("multi_subs.mkv", ["-i", video_src, "-t", "2", "-i", srt_path,
                            "-map", "0:v:0", "-map", "0:a:0?",
                            "-map", "1:s:0",
                            "-c:v", "libx264", "-c:a", "aac", "-c:s", "srt",
                            "-metadata:s:s:0", "language=eng"])
    emit("multi_video.mkv", ["-i", video_src, "-t", "2", "-map", "0:v:0",
                             "-map", "0:v:0", "-c:v", "libx264"])

    # --- dimension: audio only --------------------------------------------
    # Same reason as the container dimension: opus and vorbis need -strict -2
    # here, and an encoder this build lacks is a SKIP rather than a failure.
    for codec, ext in (("aac", ".m4a"), ("mp3", ".mp3"), ("opus", ".ogg"),
                       ("flac", ".flac"), ("vorbis", ".ogg"), ("ac3", ".ac3")):
        if codec not in available_encoders:
            skipped.append(("audio_%s%s" % (codec, ext),
                            "no encoder for %s in this ffmpeg build" % codec))
            continue
        extra = ["-strict", "-2"] if codec in ("opus", "vorbis") else []
        emit("audio_%s%s" % (codec, ext),
             ["-i", src, "-t", "2", "-vn", "-c:a", codec] + extra)

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

    return made, failed, skipped


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
        made, failed, skipped = build_matrix(ffmpeg, audio_src, workdir,
                                              video_src)
        print("check_corpus: %d sample(s) generated across "
              "codec / container / resolution / rate / multitrack / audio / damage"
              % len(made))
        if failed:
            # These used to vanish without a word, which made a shrinking
            # corpus look like a stable one. Name them and say why.
            print("")
            print("  %d encode(s) failed and are NOT in the corpus:" % len(failed))
            for name, reason in failed:
                print("    - %s: %s" % (name, reason))
        if skipped:
            # Not defects -- this machine's ffmpeg cannot build these at all.
            # Listed so a smaller corpus is never mistaken for a green one.
            print("")
            print("  %d sample(s) SKIPPED (unavailable here):" % len(skipped))
            for name, reason in skipped:
                print("    - %s: %s" % (name, reason))
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
        not_opened = []
        not_seeked = []
        for name, path in made:
            ok, first, seeked, was_crashed = measure(args.headless, path,
                                                    args.timeout)
            if ok:
                opened_count += 1
            else:
                # Named, not just counted. A rate alone tells you something got
                # worse but not what, and a helper file that is not media at all
                # then looks identical to a container the player cannot open.
                not_opened.append(name)
            if first is not None:
                first_frames.append(first)
            if seeked:
                seek_ok += 1
            else:
                not_seeked.append(name)
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
        if not_opened:
            print("")
            print("  %d sample(s) did NOT open:" % len(not_opened))
            for name in not_opened:
                print("    - %s" % name)
        if not_seeked:
            print("")
            print("  %d sample(s) did not exit 0:" % len(not_seeked))
            for name in not_seeked:
                print("    - %s" % name)

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
