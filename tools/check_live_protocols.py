#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Live-protocol coverage acceptance: RTMP, RTSP, SRT -- over a real peer.

docs/12 2.4. The HLS-VOD / HTTP-progressive half is covered by
tools/check_protocols.py against a local httpd. The three *streaming* protocols
cannot be served from a file, so they need a real streaming peer.

Two backends are supported, selected by --backend (default: auto):

  mediamtx  MediaMTX (open-source RTMP/RTSP/SRT server). The canonical peer:
            it serves all three protocols from one process, and is the only
            backend that can exercise RTMP (ffmpeg cannot act as an RTMP
            server). Requires --mediamtx <binary>.

  ffmpeg    The system ffmpeg CLI is used as a *listener* server for RTSP/SRT
            (it publishes and serves the clip in one process). This needs no
            download and is what lets the acceptance run in a bare sandbox.
            RTMP is reported SERVER_MISSING (ffmpeg has no RTMP server); RTSP
            is best-effort -- ffmpeg's built-in RTSP server is fragile across
            versions, so if it fails to come up we report SERVER_MISSING rather
            than a false FAIL.

For each protocol the script publishes/serves a short, looping clip and opens it
with ``headless``, then judges the result.

JUDGEMENT
  PASS    ``headless`` reaches kCompleted, OR the media clock it reports advances
          past --media-threshold (default 2.0 s). The threshold is the real
          assertion: frames actually traversed the protocol, not just that the
          connection opened.
  MISSING the output contains "Protocol not found" / "Unknown protocol" -- the
          protocol is not compiled into this FFmpeg build. An environment gap,
          not a player regression.
  SERVER_MISSING the chosen backend cannot provide a server for this protocol
          (RTMP under the ffmpeg backend; RTSP if ffmpeg's listener fails to
          bind). Also an environment gap -- re-run with --mediamtx to cover it.
  FAIL    the server came up but ``headless`` did not advance the clock (a
          connection refused after a good server, a demux error, a stall, a
          crash). This is the only status that means a player/behaviour
          regression.

LATENCY
  For every PASS case the script also reports L = (wall elapsed - T0) - media
  time reached. That is the end-to-end one-way-ish latency of the protocol path
  as seen by the player, and is the number docs/12 2.6 (the LL-HLS decision) is
  actually about: how much latency each protocol carries, and whether SRT's low
  figure makes a self-built LL-HLS demuxer unnecessary.

EXIT CODES
    0  every targeted protocol reached PASS
    1  at least one protocol FAILED (a player/behaviour regression)
    2  a prerequisite is missing, or one or more protocols report MISSING /
       SERVER_MISSING (environment gaps, not player regressions)
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import time

# MediaMTX ports, matching tools/mediamtx/mediamtx.yml (loopback, v1.21.x).
MEDIA_MTX_PORTS = {
    "rtmp": 1935,
    "rtsp": 8554,
    "srt": 8890,
}
# RTMP/RTSP listen on TCP; SRT listens on UDP. wait_for_port() is a TCP connect,
# so it can only gate the TCP protocols -- SRT is validated by the play attempt.
TCP_PROTOCOLS = {"rtmp", "rtsp"}
# Ports used by the ffmpeg-listener backend (kept off the MediaMTX range so the
# two backends never collide if both happen to be present).
FFMPEG_PORTS = {
    "rtmp": 11935,
    "rtsp": 18554,
    "srt": 18990,
}
HOST = "127.0.0.1"
STREAM = "avbase"


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


def _position_seconds(rest):
    """Parse the media time out of one `position: <t> / <dur>` fragment.

    Returns None when the fragment does not start with a number. headless
    writes a bare "ms" for sub-millisecond positions ("0.231000ms"), so the
    "ms" suffix is a plain no-op rather than something to scale.
    """
    rest = rest.strip()
    if not rest:
        return None
    head = rest.split()[0].rstrip("s")
    try:
        return float(head)
    except ValueError:
        return None


def parse_media_time(text):
    """Return the largest media time (seconds) reported by headless, or 0.0.

    headless prints lines of the form
        position: 12.345678s / 3.000000s (buffered ...)
    and on a finished VOD
        completed at media time 3.009570s
    The largest timestamp seen is the clock the player actually advanced to.

    The position updates are written with a carriage return and NO newline, so
    one physical line can hold a dozen of them (a live RTMP run reported
    0.23s..11s all on one line). Scanning with a single find() per line would
    therefore only ever see the first fragment and report ~0s -- which reads as
    "the player never advanced" for a stream that in fact played fine. Hence
    the repeated find() below, not a per-line parse.
    """
    best = 0.0
    for line in text.splitlines():
        cursor = 0
        while True:
            idx = line.find("position:", cursor)
            if idx < 0:
                break
            cursor = idx + len("position:")
            value = _position_seconds(line[cursor:])
            if value is not None:
                best = max(best, value)
        idx = line.find("completed at media time")
        if idx >= 0:
            value = _position_seconds(line[idx + len("completed at media time"):])
            if value is not None:
                best = max(best, value)
    return best


# --------------------------------------------------------------------------
# MediaMTX backend
# --------------------------------------------------------------------------

def publish_command_mediamtx(ffmpeg, source, protocol, port):
    """ffmpeg CLI command that publishes |source| to MediaMTX over |protocol|.

    ``-re`` publishes at the clip's native rate (so a live reader sees a real
    stream) and ``-c copy`` keeps it a pure protocol test -- no transcode.

    ``-stream_loop -1`` loops forever: the read side is bounded by headless's
    own ``--timeout``, and a *finite* loop would restart the clip underneath a
    reader that is still attached. RTMP/SRT readers do not survive that well --
    sequence numbers reset mid-stream -- and the result would be attributed to
    the player rather than to the publisher's loop count.
    """
    base = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-re",
        "-stream_loop", "-1", "-i", source, "-c", "copy",
    ]
    if protocol == "rtmp":
        return base + ["-f", "flv", "rtmp://%s:%d/%s" % (HOST, port, STREAM)]
    if protocol == "rtsp":
        return base + ["-f", "rtsp", "rtsp://%s:%d/%s" % (HOST, port, STREAM)]
    if protocol == "srt":
        # MediaMTX addresses SRT paths through the streamid, and the two
        # directions are separated by a COLON ("publish:foo" / "read:foo").
        # A slash makes MediaMTX reject the handshake ("Peer rejected
        # connection"), which looks exactly like a player failure.
        url = "srt://%s:%d?streamid=publish:%s" % (HOST, port, STREAM)
        return base + ["-f", "mpegts", url]
    raise ValueError("unknown protocol: %s" % protocol)


def play_url_mediamtx(protocol, port):
    if protocol == "rtmp":
        return "rtmp://%s:%d/%s" % (HOST, port, STREAM)
    if protocol == "rtsp":
        return "rtsp://%s:%d/%s" % (HOST, port, STREAM)
    if protocol == "srt":
        return "srt://%s:%d?streamid=read:%s" % (HOST, port, STREAM)
    raise ValueError("unknown protocol: %s" % protocol)


def wait_until_readable(ffmpeg, url, timeout):
    """Wait until |url| actually serves data, or return False.

    A fixed sleep between publisher and reader is a race: RTMP needs a publish
    handshake before MediaMTX will serve the path at all, and the time that
    takes is not bounded by anything we control. Reading it back with ffprobe
    turns "is it ready yet?" into an observable condition.

    This is deliberately ffprobe and not headless: the question here is whether
    the *peer* is serving, and only then is it fair to hold headless's clock
    against it. A peer that never comes up is an environment gap
    (SERVER_MISSING), not a player regression.
    """
    probe = os.path.join(os.path.dirname(ffmpeg), "ffprobe")
    if not os.path.exists(probe):
        probe = ffmpeg  # degrade to a bounded open attempt
    deadline = time.time() + timeout
    while True:
        try:
            done = subprocess.run(
                [probe, "-v", "error", "-rw_timeout", "3000000",
                 "-show_entries", "format=format_name",
                 "-of", "default=nw=1", url],
                capture_output=True, text=True, timeout=6)
            if done.returncode == 0 and "format_name" in done.stdout:
                return True
        except subprocess.TimeoutExpired:
            pass
        if time.time() >= deadline:
            return False
        time.sleep(0.3)


def run_case_mediamtx(headless, ffmpeg, source, protocol, port, play_budget,
                      media_threshold):
    """Publish to MediaMTX over |protocol|, play it back, judge the result.

    Returns (status, media_time, latency, detail).
    """
    pub = publish_command_mediamtx(ffmpeg, source, protocol, port)
    publisher = subprocess.Popen(pub, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
    try:
        # The reader must come after the writer is up: MediaMTX only serves a
        # path once something is publishing to it.
        url = play_url_mediamtx(protocol, port)
        if not wait_until_readable(ffmpeg, url, timeout=15.0):
            return ("SERVER_MISSING", 0.0, 0.0,
                    "publisher never made %s readable" % url)
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
            # own budget; a timeout here is expected and re-interpreted below.
            elapsed = time.time() - t0
            out = ""
            status = -9

        media_time = parse_media_time(out)
        latency = elapsed - media_time

        if "Protocol not found" in out or "Unknown protocol" in out:
            return ("MISSING", media_time, latency,
                    "protocol not compiled into this FFmpeg build")
        if media_time >= media_threshold:
            return ("PASS", media_time, latency,
                    "media clock advanced %.3fs" % media_time)
        lines = [l for l in out.splitlines() if l.strip()]
        detail = "\n      ".join(lines[-6:]) if lines else (
            "no output; headless exit %s, media time %.3fs" %
            (status, media_time))
        return ("FAIL", media_time, latency, detail)
    finally:
        publisher.terminate()
        try:
            publisher.wait(timeout=5)
        except subprocess.TimeoutExpired:
            publisher.kill()


# --------------------------------------------------------------------------
# ffmpeg-listener backend
# --------------------------------------------------------------------------

def start_ffmpeg_listener(ffmpeg, source, protocol, port):
    """Start ffmpeg serving |source| as a listener for |protocol|.

    Returns the Popen, or None if the protocol cannot be served this way
    (RTMP -- ffmpeg has no RTMP server -- or RTSP if the listener fails to
    bind). The returned process both publishes and serves; there is no separate
    publisher.
    """
    if protocol == "rtmp":
        return None
    base = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-re",
        "-stream_loop", "-1", "-i", source, "-c", "copy",
    ]
    if protocol == "srt":
        # SRT is UDP; the listener binds a UDP socket. We cannot wait_for_port
        # (TCP), so we just confirm the process survived its startup.
        cmd = base + ["-f", "mpegts",
                      "srt://%s:%d?mode=listener" % (HOST, port)]
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        if p.poll() is not None:
            return None
        return p
    if protocol == "rtsp":
        cmd = base + ["-f", "rtsp", "-rtsp_flags", "listen",
                      "rtsp://%s:%d/%s" % (HOST, port, STREAM)]
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        # RTSP is TCP; if the listener fails to bind (ffmpeg's built-in RTSP
        # server is fragile) the process exits quickly -- detect that.
        if not wait_for_port(port, timeout=5.0) or p.poll() is not None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
            return None
        return p
    raise ValueError("unknown protocol: %s" % protocol)


def play_url_ffmpeg(protocol, port):
    if protocol == "srt":
        return "srt://%s:%d" % (HOST, port)
    if protocol == "rtsp":
        return "rtsp://%s:%d/%s" % (HOST, port, STREAM)
    raise ValueError("unknown protocol: %s" % protocol)


def run_case_ffmpeg(headless, ffmpeg, source, protocol, port, play_budget,
                    media_threshold):
    """Serve |source| via ffmpeg-listener and play it back with headless."""
    server = start_ffmpeg_listener(ffmpeg, source, protocol, port)
    if server is None:
        return ("SERVER_MISSING", 0.0, 0.0,
                "ffmpeg cannot serve %s (use --mediamtx for a real server)" %
                protocol)
    try:
        url = play_url_ffmpeg(protocol, port)
        t0 = time.time()
        try:
            result = subprocess.run(
                [headless, url, "--timeout", str(play_budget)],
                capture_output=True, text=True, timeout=play_budget + 30)
            elapsed = time.time() - t0
            out = result.stdout + result.stderr
            status = result.returncode
        except subprocess.TimeoutExpired:
            elapsed = time.time() - t0
            out = ""
            status = -9

        media_time = parse_media_time(out)
        latency = elapsed - media_time

        if "Protocol not found" in out or "Unknown protocol" in out:
            return ("MISSING", media_time, latency,
                    "protocol not compiled into this FFmpeg build")
        if media_time >= media_threshold:
            return ("PASS", media_time, latency,
                    "media clock advanced %.3fs" % media_time)
        lines = [l for l in out.splitlines() if l.strip()]
        detail = "\n      ".join(lines[-6:]) if lines else (
            "no output; headless exit %s, media time %.3fs" %
            (status, media_time))
        return ("FAIL", media_time, latency, detail)
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


# --------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--headless", required=True)
    parser.add_argument("--mediamtx", default=None,
                        help="path to the mediamtx binary (enables the "
                             "mediamtx backend; required for RTMP)")
    parser.add_argument("--ffmpeg", default="ffmpeg",
                        help="ffmpeg CLI (default: resolved from PATH)")
    parser.add_argument("--backend", default="auto",
                        choices=["auto", "mediamtx", "ffmpeg"],
                        help="server backend (default: auto -- mediamtx if "
                             "present, else ffmpeg-listener)")
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
    ffmpeg = shutil.which(args.ffmpeg) or (
        args.ffmpeg if os.path.exists(args.ffmpeg) else None)
    if not ffmpeg:
        print("check_live_protocols: ffmpeg CLI not found (%s)" % args.ffmpeg)
        return 2

    mediamtx = None
    if args.mediamtx:
        mediamtx = shutil.which(args.mediamtx) or (
            args.mediamtx if os.path.exists(args.mediamtx) else None)
        if not mediamtx:
            print("check_live_protocols: mediamtx binary not found: %s" %
                  args.mediamtx)
            return 2

    # Resolve backend.
    if args.backend == "mediamtx":
        if not mediamtx:
            print("check_live_protocols: --backend=mediamtx requires "
                  "--mediamtx <binary>")
            return 2
        backend = "mediamtx"
    elif args.backend == "ffmpeg":
        backend = "ffmpeg"
    else:  # auto
        backend = "mediamtx" if mediamtx else "ffmpeg"

    config = args.mediamtx_config or find_mediamtx_config()
    if backend == "mediamtx" and (not config or not os.path.exists(config)):
        print("check_live_protocols: mediamtx config not found: %s" % config)
        return 2

    source = os.path.join(args.testdata, "small_h264_aac_3s.mp4")
    if not os.path.exists(source):
        print("check_live_protocols: source media not found: %s" % source)
        return 2

    protocols = [p.strip().lower() for p in args.protocols.split(",") if p.strip()]
    unknown = [p for p in protocols if p not in MEDIA_MTX_PORTS]
    if unknown:
        print("check_live_protocols: unknown protocol(s): %s" %
              ", ".join(unknown))
        return 2

    ports = MEDIA_MTX_PORTS if backend == "mediamtx" else FFMPEG_PORTS

    print("check_live_protocols: docs/12 2.4 -- live protocol coverage")
    print("  backend  : %s" % backend)
    print("  ffmpeg   : %s" % ffmpeg)
    if backend == "mediamtx":
        print("  mediamtx : %s" % mediamtx)
        print("  config   : %s" % config)
    print("  source   : %s" % source)

    server = None
    if backend == "mediamtx":
        server = subprocess.Popen([mediamtx, config],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        # MediaMTX brings up all listeners at once; wait on the first TCP
        # listener requested (skipped entirely if only SRT is requested -- a UDP
        # port cannot be probed with a TCP connect).
        tcp_protos = [p for p in protocols if p in TCP_PROTOCOLS]
        if tcp_protos and not wait_for_port(ports[tcp_protos[0]], timeout=10.0):
            print("check_live_protocols: MediaMTX did not come up on port %d" %
                  ports[tcp_protos[0]])
            server.terminate()
            return 2

    try:
        results = []
        for protocol in protocols:
            port = ports[protocol]
            if (backend == "mediamtx" and protocol in TCP_PROTOCOLS
                    and not wait_for_port(port, timeout=10.0)):
                results.append((protocol, "FAIL", 0.0, 0.0,
                                "port %d not listening" % port))
                continue
            if backend == "mediamtx":
                r = run_case_mediamtx(args.headless, ffmpeg, source, protocol,
                                      port, args.play_budget,
                                      args.media_threshold)
            else:
                r = run_case_ffmpeg(args.headless, ffmpeg, source, protocol,
                                    port, args.play_budget, args.media_threshold)
            results.append((protocol,) + r)

        print("")
        print("  %-8s %-14s %-10s %s" %
              ("PROTO", "STATUS", "MEDIA[s]", "LATENCY[s]"))
        any_fail = False
        any_envgap = False
        for protocol, status, media_time, latency, detail in results:
            print("  %-8s %-14s %-10.3f %.3f" %
                  (protocol.upper(), status, media_time, latency))
            if detail:
                print("      %s" % detail)
            if status == "FAIL":
                any_fail = True
            elif status in ("MISSING", "SERVER_MISSING"):
                any_envgap = True

        print("")
        if not any_fail and not any_envgap:
            print("check_live_protocols: all targeted protocols played "
                  "(media clock advanced past %.1fs)" % args.media_threshold)
            return 0
        if any_fail:
            print("check_live_protocols: FAILED (a player/behaviour regression)")
            return 1
        # only environment gaps remain
        print("check_live_protocols: environment gaps only (MISSING / "
              "SERVER_MISSING) -- re-run with --mediamtx to cover them")
        return 2
    finally:
        if server is not None:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()


if __name__ == "__main__":
    sys.exit(main())
