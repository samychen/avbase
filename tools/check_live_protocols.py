#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Live-protocol coverage acceptance: RTMP, RTSP, SRT -- over a real peer.

docs/12 2.4. The HLS-VOD / HTTP-progressive half is covered by
tools/check_protocols.py against a local httpd. The three *streaming* protocols
cannot be served from a file, so they need a real streaming peer. MediaMTX is
that peer: an open-source RTMP/RTSP/SRT server. This script owns its whole
lifecycle for the duration of the run.

For each protocol it:
  1. publishes a short, looping clip to MediaMTX with the ffmpeg CLI
     (``-stream_loop 2`` so a 3 s clip yields ~9 s of real-time source -- a live
     source, by construction, never reaches kCompleted),
  2. opens that published stream with ``headless`` and lets it play for a budget,
  3. judges the result.

JUDGEMENT
  PASS    ``headless`` reaches kCompleted, OR the media clock it reports advances
          past --media-threshold (default 2.0 s). The threshold is the real
          assertion: frames actually traversed the protocol, not just that the
          connection opened.
  MISSING the output contains "Protocol not found" / "Unknown protocol" -- the
          protocol is not compiled into this FFmpeg build. An environment gap,
          not a player regression.
  FAIL    anything else that did not PASS (a connection refused, a demux error, a
          stall, a crash -- observed as headless not advancing the clock and
          exiting with a non-zero / killed status).

LATENCY
  For every PASS case the script also reports L = (wall elapsed - T0) - media
  time reached. That is the end-to-end one-way-ish latency of the protocol path
  as seen by the player, and is the number docs/12 2.6 (the LL-HLS decision)
  is actually about: how much latency each protocol carries, and whether SRT's
  low figure makes a self-built LL-HLS demuxer unnecessary.

EXIT CODES
    0  every targeted protocol reached PASS
    1  at least one protocol FAILED (a player/behaviour regression)
    2  a prerequisite is missing (mediamtx binary, ffmpeg CLI, source media) or
       one or more protocols report MISSING (not compiled into the build)
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import time

# MediaMTX ports, matching tools/mediamtx/mediamtx.yml (loopback, v1.21.x).
DEFAULT_PORTS = {
    "rtmp": 1935,
    "rtsp": 8554,
    "srt": 8890,
}
HOST = "127.0.0.1"


def wait_for_port(port, timeout=10.0):
    """Block until |port| accepts a TCP connection, or return False."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((HOST, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def find_mediamtx_config():
    here = os.path.dirname(os.path.abspath(__file__))
    candidate = os.path.join(here, "mediamtx", "mediamtx.yml")
    return candidate if os.path.exists(candidate) else None


def publish_command(ffmpeg, source, protocol, port, stream_name):
    """ffmpeg CLI command that publishes |source| to MediaMTX over |protocol|.

    ``-re`` publishes at the clip's native rate (so a live reader sees a real
    stream), ``-stream_loop 2`` repeats the 3 s clip so the source outlives the
    read budget. ``-c copy`` keeps it a pure protocol test -- no transcode.
    """
    base = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-re",
        "-stream_loop", "2", "-i", source, "-c", "copy",
    ]
    if protocol == "rtmp":
        return base + ["-f", "flv", "rtmp://%s:%d/%s" % (HOST, port, stream_name)]
    if protocol == "rtsp":
        return base + ["-f", "rtsp", "rtsp://%s:%d/%s" % (HOST, port, stream_name)]
    if protocol == "srt":
        # MediaMTX SRT: the streamid carries the direction and path.
        url = "srt://%s:%d?streamid=publish/%s" % (HOST, port, stream_name)
        return base + ["-f", "mpegts", url]
    raise ValueError("unknown protocol: %s" % protocol)


def play_url(protocol, port, stream_name):
    if protocol == "rtmp":
        return "rtmp://%s:%d/%s" % (HOST, port, stream_name)
    if protocol == "rtsp":
        return "rtsp://%s:%d/%s" % (HOST, port, stream_name)
    if protocol == "srt":
        return "srt://%s:%d?streamid=read/%s" % (HOST, port, stream_name)
    raise ValueError("unknown protocol: %s" % protocol)


def parse_media_time(text):
    """Return the largest media time (seconds) reported by headless, or 0.0.

    headless prints lines of the form
        position: 12.345678s / 3.000000s (buffered ...)
    and on a finished VOD
        completed at media time 3.009570s
    The largest timestamp seen is the clock the player actually advanced to.
    """
    best = 0.0
    for line in text.splitlines():
        idx = line.find("position:")
        if idx >= 0:
            # "position: 12.345678s / 3.000000s ..."
            rest = line[idx + len("position:"):].strip()
            head = rest.split()[0] if rest.split() else ""
            head = head.rstrip("s")
            try:
                best = max(best, float(head))
            except ValueError:
                pass
            continue
        idx = line.find("completed at media time")
        if idx >= 0:
            head = line[idx + len("completed at media time"):].strip()
            head = head.rstrip("s")
            try:
                best = max(best, float(head))
            except ValueError:
                pass
    return best


def run_case(headless, ffmpeg, source, mediamtx, config, protocol, port,
             play_budget, media_threshold, stream_name):
    """Publish over |protocol|, play it back, and judge the result.

    Returns (status, media_time, latency, detail) where status is one of
    "PASS" / "MISSING" / "FAIL".
    """
    pub = publish_command(ffmpeg, source, protocol, port, stream_name)
    # Start the publisher first: MediaMTX only serves a path once something is
    # publishing to it, so the reader must come after the writer is up.
    publisher = subprocess.Popen(pub, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
    try:
        # Give the publisher a head start to open the stream before the reader
        # connects -- a reader that beats the publisher gets "no one is
        # publishing" and the whole case is mis-classified as a player bug.
        time.sleep(1.5)
        url = play_url(protocol, port, stream_name)
        t0 = time.time()
        try:
            result = subprocess.run(
                [headless, url, "--timeout", str(play_budget)],
                capture_output=True, text=True, timeout=play_budget + 30)
            elapsed = time.time() - t0
            out = result.stdout + result.stderr
            status = result.returncode
        except subprocess.TimeoutExpired:
            # A live loop never reaches kCompleted, so we drive the stop with our
            # own budget; a timeout here is expected and is re-interpreted below.
            elapsed = time.time() - t0
            out = ""
            status = -9  # stand-in for "killed by budget", not a hard error

        media_time = parse_media_time(out)
        latency = elapsed - media_time

        if "Protocol not found" in out or "Unknown protocol" in out:
            return ("MISSING", media_time, latency,
                    "protocol not compiled into this FFmpeg build")
        if status == 0 and media_time >= media_threshold:
            return ("PASS", media_time, latency,
                    "kCompleted (media time %.3fs)" % media_time)
        if media_time >= media_threshold:
            # Advanced far enough despite a non-zero exit (e.g. killed at the
            # budget after reaching the threshold) -- still a pass.
            return ("PASS", media_time, latency,
                    "media clock advanced %.3fs" % media_time)
        # Did not advance the clock: a real failure.
        lines = [l for l in out.splitlines() if l.strip()]
        detail = "\n      ".join(lines[-6:]) if lines else (
            "no output; headless exit %s, media time %.3fs" % (status, media_time))
        return ("FAIL", media_time, latency, detail)
    finally:
        publisher.terminate()
        try:
            publisher.wait(timeout=5)
        except subprocess.TimeoutExpired:
            publisher.kill()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--headless", required=True)
    parser.add_argument("--mediamtx", required=True,
                        help="path to the mediamtx binary")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--mediamtx-config", default=None,
                        help="mediamtx.yml (default: tools/mediamtx/mediamtx.yml)")
    parser.add_argument("--testdata", default="tests/testdata")
    parser.add_argument("--protocols", default="rtmp,rtsp,srt")
    parser.add_argument("--play-budget", type=int, default=12,
                        help="seconds headless is allowed to read each stream")
    parser.add_argument("--media-threshold", type=float, default=2.0,
                        help="media seconds that must be reached to count PASS")
    args = parser.parse_args()

    if not os.path.exists(args.headless):
        print("check_live_protocols: headless binary not found: %s" %
              args.headless)
        return 2
    mediamtx = shutil.which(args.mediamtx) or (
        args.mediamtx if os.path.exists(args.mediamtx) else None)
    if not mediamtx:
        print("check_live_protocols: mediamtx binary not found: %s" %
              args.mediamtx)
        return 2
    ffmpeg = shutil.which(args.ffmpeg) or (
        args.ffmpeg if os.path.exists(args.ffmpeg) else None)
    if not ffmpeg:
        print("check_live_protocols: ffmpeg CLI not found (%s)" % args.ffmpeg)
        return 2
    config = args.mediamtx_config or find_mediamtx_config()
    if not config or not os.path.exists(config):
        print("check_live_protocols: mediamtx config not found: %s" % config)
        return 2
    source = os.path.join(args.testdata, "small_h264_aac_3s.mp4")
    if not os.path.exists(source):
        print("check_live_protocols: source media not found: %s" % source)
        return 2

    protocols = [p.strip().lower() for p in args.protocols.split(",") if p.strip()]
    unknown = [p for p in protocols if p not in DEFAULT_PORTS]
    if unknown:
        print("check_live_protocols: unknown protocol(s): %s" % ", ".join(unknown))
        return 2

    # Bring MediaMTX up.
    print("check_live_protocols: docs/12 2.4 -- live protocol coverage (MediaMTX)")
    print("  mediamtx : %s" % mediamtx)
    print("  config   : %s" % config)
    print("  ffmpeg   : %s" % ffmpeg)
    print("  source   : %s" % source)

    server = subprocess.Popen([mediamtx, "--config", config],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    try:
        # Wait for the first enabled port to accept connections before trusting
        # the server is listening on all of them.
        first_port = DEFAULT_PORTS[protocols[0]]
        if not wait_for_port(first_port, timeout=10.0):
            print("check_live_protocols: MediaMTX did not come up on port %d" %
                  first_port)
            return 2

        results = []
        for protocol in protocols:
            port = DEFAULT_PORTS[protocol]
            if not wait_for_port(port, timeout=10.0):
                print("  %-8s %s" % (protocol.upper(),
                                     "FAILED (port %d not listening)" % port))
                results.append((protocol, "FAIL", 0.0, 0.0,
                                "port %d not listening" % port))
                continue
            status, media_time, latency, detail = run_case(
                args.headless, ffmpeg, source, mediamtx, config, protocol, port,
                args.play_budget, args.media_threshold, "avbase")
            results.append((protocol, status, media_time, latency, detail))

        print("")
        print("  %-8s %-8s %-10s %s" % ("PROTO", "STATUS", "MEDIA[s]", "LATENCY[s]"))
        any_fail = False
        any_missing = False
        for protocol, status, media_time, latency, detail in results:
            print("  %-8s %-8s %-10.3f %.3f" %
                  (protocol.upper(), status, media_time, latency))
            if detail:
                print("      %s" % detail)
            if status == "FAIL":
                any_fail = True
            elif status == "MISSING":
                any_missing = True

        print("")
        if not any_fail and not any_missing:
            print("check_live_protocols: all targeted protocols played "
                  "(media clock advanced past %.1fs)" % args.media_threshold)
            return 0
        if any_missing:
            print("check_live_protocols: at least one protocol is MISSING from "
                  "this FFmpeg build (see above)")
            return 2
        print("check_live_protocols: FAILED")
        return 1
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


if __name__ == "__main__":
    sys.exit(main())
