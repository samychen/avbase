#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Protocol coverage acceptance: HLS VOD and HTTP progressive, over a real socket.

docs/12 2.4. The claim under test is that the DataSource bridge and the demuxer
handle a NETWORK source, not just a memory buffer. That claim cannot be made
good by a file:// URL: the interesting behaviour is in the bridge's offset
bookkeeping, its handling of a source that cannot satisfy a read instantly, and
the seek path against a source that answers AVSEEK_SIZE.

Everything here is local by construction -- a python http.server on a loopback
port, media generated from tests/testdata at run time -- so this is a NETWORK
test that needs no network. RTMP/RTSP/SRT cannot be covered this way and are
registered as a separate, environment-dependent run.

USAGE
    tools/check_protocols.py --headless build/ffmpeg/bin/headless \\
                             --ffmpeg /path/to/ffmpeg \\
                             --testdata tests/testdata

EXIT CODES
    0  every protocol reached kCompleted
    1  at least one failed (each is reported with the tail of its output)
    2  a prerequisite is missing (binary, ffmpeg CLI, or the source media)
"""

import argparse
import http.server
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading

# A port the test owns. Chosen by binding to 0 and asking the OS, because a
# fixed port is a fixed way for two CI runs on one host to collide.
def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    """Static file server WITH HTTP Range support, and without request logging.

    THE RANGE SUPPORT IS NOT OPTIONAL, and finding that out is the reason this
    class exists rather than the stock SimpleHTTPRequestHandler.

    docs/12 2.4 says "HTTP progressive: same deal, a local httpd". It is not the
    same deal. A progressive MP4 is probed by seeking -- the demuxer reads the
    head, then jumps to the moov atom, then reads media by range -- so a server
    that answers every request with 200 + the whole file cannot serve it. The
    stock handler is exactly such a server: it has no Range support at all.

    The symptom is nasty enough to be worth recording. HLS played fine (each
    .ts segment is self-contained and read front to back, so it never needs a
    range), and progressive MP4 HUNG until the timeout and then tripped a
    CHECK in the shutdown path. Nothing in that output says "your test server
    does not implement RFC 7233"; it looks like a player bug, and it would
    have been filed as one.

    Only the single-range form ("bytes=N-") is implemented, because that is all
    a media demuxer asks for. Multipart ranges would be more faithful to the RFC
    and would prove nothing extra here.
    """

    def log_message(self, fmt, *args):
        pass

    def send_head(self):
        header = self.headers.get("Range")
        if not header or not header.startswith("bytes="):
            return super().send_head()
        path = self.translate_path(self.path)
        if os.path.isdir(path):
            return super().send_head()
        try:
            handle = open(path, "rb")
        except OSError:
            self.send_error(404, "File not found")
            return None
        size = os.fstat(handle.fileno()).st_size
        spec = header[len("bytes="):].split(",")[0].strip()
        start_text, _, end_text = spec.partition("-")
        try:
            start = int(start_text) if start_text else 0
            end = int(end_text) if end_text else size - 1
        except ValueError:
            handle.close()
            self.send_error(400, "Malformed Range")
            return None
        if start >= size:
            handle.close()
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % size)
            self.end_headers()
            return None
        end = min(end, size - 1)
        length = end - start + 1
        handle.seek(start)
        self.send_response(206)
        self.send_header("Content-type", self.guess_type(path))
        self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.send_header("Content-Length", str(length))
        self.end_headers()
        return _RangeReader(handle, length)


class _RangeReader:
    """A file-like object limited to |length| bytes, for the copy loop."""

    def __init__(self, handle, length):
        self._handle = handle
        self._remaining = length

    def read(self, size=-1):
        if self._remaining <= 0:
            return b""
        if size is None or size < 0 or size > self._remaining:
            size = self._remaining
        chunk = self._handle.read(size)
        self._remaining -= len(chunk)
        return chunk

    def close(self):
        self._handle.close()


def wait_for_port(port, timeout=10.0):
    import time
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def make_hls(ffmpeg, source, outdir):
    """Writes a VOD playlist plus its segments. Returns the playlist path."""
    playlist = os.path.join(outdir, "index.m3u8")
    result = subprocess.run(
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", source,
         "-c", "copy", "-f", "hls", "-hls_time", "1", "-hls_list_size", "0",
         "-hls_segment_filename", os.path.join(outdir, "seg%02d.ts"), playlist],
        capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(playlist):
        return None, result.stderr.strip()
    return playlist, None


def run_case(headless, url, timeout, label):
    result = subprocess.run([headless, url, "--timeout", str(timeout)],
                            capture_output=True, text=True, timeout=timeout + 30)
    ok = result.returncode == 0
    detail = ""
    if not ok:
        lines = [l for l in (result.stdout + result.stderr).splitlines() if l.strip()]
        detail = "\n      ".join(lines[-6:])
    print("  %-34s %s" % (label, "ok" if ok else "FAILED (exit %d)"
                          % result.returncode))
    if detail:
        print("      %s" % detail)
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--headless", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--testdata", default="tests/testdata")
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()

    if not os.path.exists(args.headless):
        print("check_protocols: headless binary not found: %s" % args.headless)
        return 2
    ffmpeg = shutil.which(args.ffmpeg) or (
        args.ffmpeg if os.path.exists(args.ffmpeg) else None)
    if not ffmpeg:
        print("check_protocols: ffmpeg CLI not found (%s)" % args.ffmpeg)
        print("  needed to GENERATE the HLS assets; the point of the test is")
        print("  that the media is produced locally, not fetched.")
        return 2
    source = os.path.join(args.testdata, "small_h264_aac_3s.mp4")
    if not os.path.exists(source):
        print("check_protocols: source media not found: %s" % source)
        return 2

    workdir = tempfile.mkdtemp(prefix="avbase-protocols-")
    try:
        playlist, err = make_hls(ffmpeg, source, workdir)
        if playlist is None:
            print("check_protocols: could not build HLS assets: %s" % err)
            return 2
        # A plain copy for the progressive case: the same bytes, served without
        # a playlist, which is the shape an HTTP progressive source actually
        # has (and the shape that exercises the bridge's offset bookkeeping
        # rather than HLS's segment logic).
        shutil.copy(source, os.path.join(workdir, "plain.mp4"))
        segments = [f for f in os.listdir(workdir) if f.endswith(".ts")]
        print("check_protocols: generated %d HLS segment(s) + a progressive copy"
              % len(segments))

        port = free_port()
        handler = lambda *a, **kw: QuietHandler(*a, directory=workdir, **kw)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        if not wait_for_port(port):
            print("check_protocols: local httpd did not come up on %d" % port)
            return 2

        base = "http://127.0.0.1:%d" % port
        try:
            print("check_protocols: docs/12 2.4 -- protocol coverage")
            results = [
                run_case(args.headless, base + "/index.m3u8", args.timeout,
                         "HLS VOD (playlist + segments)"),
                run_case(args.headless, base + "/plain.mp4", args.timeout,
                         "HTTP progressive (single file)"),
            ]
        finally:
            server.shutdown()
            server.server_close()

        print("")
        if all(results):
            print("check_protocols: both protocols reached kCompleted")
            print("")
            print("  NOT covered here, and why: RTMP, RTSP and SRT cannot be")
            print("  served from a file, so they need a real peer and a real")
            print("  network path. They stay a separate, environment-dependent")
            print("  run (docs/12 2.4) rather than a pretend local one.")
            return 0
        print("check_protocols: FAILED")
        return 1
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
