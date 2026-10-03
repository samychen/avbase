#!/usr/bin/env python3
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Degraded-network acceptance: the player must survive a bad source.

docs/12 2.5. 2.4 proved the DataSource bridge and the demuxer handle a NETWORK
source at all. That is the easy half: a loopback socket on a machine with no
load on it is the friendliest network in the world. This file asks the harder
half -- what happens when the source stops answering, answers slowly, or drops
the connection mid-playback.

THE IMPAIRMENT IS IN THE SERVER, NOT IN tc. The obvious implementation is
`tc qdisc add dev lo root netem ...`, and it is the wrong one for a gate:
netem needs root, does not exist on macOS, and therefore could only ever run in
CI -- which is exactly the "a gate only CI can enforce" shape that lets gates
rot (the same argument check_protocols.py makes for keeping its httpd local).
The server here is already ours, so the damage is applied where the bytes
actually are: it sleeps before answering, it stalls for whole seconds at a
chosen media offset, and it hangs up mid-response. Every platform that can run
the player can run this.

WHAT IS ASSERTED. Not "playback succeeded" -- under a total outage there is
nothing to succeed at. The assertion is the one that matters for a player
facing a bad network:

    the process reaches a DEFINED end state, on its own, within a bounded time

Concretely, per case: exit code 0 (kCompleted) or 1 (kError) are both
acceptable; a signal exit, a CHECK failure, a sanitizer report, or the timeout
firing are not. A player that hangs on a dead socket is broken in a way no
amount of correct decoding elsewhere excuses, and it is the failure this file
exists to catch.

USAGE
    tools/check_weaknet.py --headless build/ffmpeg/bin/headless \\
                           --ffmpeg /path/to/ffmpeg \\
                           --testdata tests/testdata

EXIT CODES
    0  every case reached a defined end state
    1  at least one hung, crashed, or hit a sanitizer
    2  a prerequisite is missing (binary, ffmpeg CLI, or the source media)
"""

import argparse
import http.server
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# The Range-capable handler and the port helper are shared with check_protocols
# rather than copied. Duplicating them would be how the two files drift: the
# Range support took a real bug to get right, and a second copy of "almost the
# same" server is a second chance to ship one without it.
from check_protocols import QuietHandler, free_port, wait_for_port  # noqa: E402

# The clip the cases play. Long enough that a multi-second outage lands in the
# middle of playback instead of before the first frame -- an outage during
# PrepareSync is a different code path (the source is opened synchronously) and
# proves nothing about recovery.
CLIP_SECONDS = 20
# How long each case may take beyond the clip's own duration before it is
# called a hang. Generous, because a stalled socket burns its own timeout first;
# the point is to catch "never returns", not to be tight about "slow".
SLACK_SECONDS = 45

# Stated, not absorbed. A dropped or stalled connection currently ends playback
# in kError rather than being retried, and the reason is worth writing down
# because it is the same shape as an earlier finding in this repo: the retry
# component exists, is unit-tested, and is not on the production path.
#     media/filters/retry_data_source.{h,cc}  -- implemented, 8 cases
#     grep -rl retry_data_source.h          -- only its own .cc and its own test
# So the gate asserts what is actually true today -- a bad network is survivable
# and does not hang or crash -- and prints this instead of quietly accepting it.
GAP_RETRY = ("GAP: ended in kError instead of resuming. RetryDataSource is "
             "implemented and unit-tested but no production header includes it, "
             "so nothing retries a dropped connection. docs/12 2.5.")


class Impairment:
    """Shared, mutable impairment state read by the request handler.

    A plain object rather than handler attributes because the handler runs on
    the server's thread and the driver mutates these from the test's thread.
    Every field is read/written as a single attribute assignment, which is
    atomic enough for "one number the test flips mid-playback" and does not
    pretend to be more than that.
    """

    def __init__(self):
        self.latency_ms = 0
        # Stall once this many bytes have gone out, for stall_seconds. Armed by
        # BYTES rather than by wall clock on purpose: a wall-clock trigger
        # assumes the player starts pulling at a predictable moment, and it does
        # not -- it probes, seeks to the moov atom, and only then begins the bulk
        # read, so a timer set for "3s in" can land in a gap where the server is
        # sending nothing at all and the outage never touches a real transfer.
        # "Halfway through the bytes" is both deterministic and exactly what an
        # outage is supposed to mean.
        self.stall_after_bytes = 0
        self.stall_seconds = 0.0
        self.stall_until = 0.0
        self.stall_done = False
        self.drop_every = 0        # 1 in N responses is hung up on
        # Bytes per second, or 0 for unlimited. THIS is the impairment that
        # matters, and finding that out is what the first version of this file
        # got wrong: with a loopback socket the player pulled the whole 20 s
        # clip in three requests and finished buffering before playback had
        # really begun, so the outage at t=4s hit a player that already had
        # every byte. Latency and packet loss are what a network does; a source
        # that is faster than real time is what a LOCAL MACHINE does, and it is
        # the one condition under which none of this is a test.
        self.bytes_per_second = 0
        self.throttled_bytes = 0
        self.requests = 0
        self.stalls_served = 0
        self.drops_served = 0


class ImpairedHandler(QuietHandler):
    """QuietHandler plus latency / stall / hangup, driven by |state|.

    QuietHandler is subclassed rather than wrapped so Range support keeps
    working: a progressive MP4 is read by seeking, and an impairment layer that
    broke Range would turn every case into "the server is wrong" instead of
    "the network is bad".
    """

    state = None            # set by serve(); one server per process run

    def handle_one_request(self):
        """Swallow only the disconnect noise, and only from the client side.

        The player closes the socket as soon as it has the last byte, so the
        server is guaranteed to be mid-send when the process exits. Python's
        default handler prints a full traceback for that, which buries the one
        line the reader actually came for. A REAL server-side failure is still
        reported -- the filter is on the exception type, not on "is it ugly".
        """
        # ValueError is in this list on purpose: the base class flushes wfile at
        # the end of handle_one_request, and once the player is gone that flush
        # raises "I/O operation on closed file" rather than a socket error. Same
        # event -- the client left mid-response -- reported through a different
        # exception type by the stdlib.
        gone = (ConnectionResetError, BrokenPipeError, ConnectionAbortedError,
                ValueError)
        try:
            super().handle_one_request()
        except gone:
            self.close_connection = True

    def handle_error(self, request, client_address):
        """Silence the copy-class disconnect, keep everything else."""
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionResetError, BrokenPipeError,
                            ConnectionAbortedError, ValueError)):
            return
        super().handle_error(request, client_address)

    def copyfile(self, source, outputfile):
        """The same copy, metered, and interruptible mid-stream.

        SimpleHTTPRequestHandler.copyfile is a plain shutil.copyfileobj. Two
        things are wrong with that for this test, and both are the reason the
        first version of this file proved nothing:

        * On loopback it moves the entire clip in one gulp, so the player
          finishes buffering before playback begins and no amount of damage
          downstream matters. Hence the token bucket: below real time, reading
          and rendering have to interleave.
        * A network that goes quiet does not wait for the next request to
          arrive -- it goes quiet in the middle of the one in flight. Blocking
          only in do_GET() therefore stalls nothing, because a throttled
          progressive MP4 makes three requests and then reads for the rest of
          the clip. Hence the per-chunk stall check below.
        """
        st = type(self).state
        metered = bool(st.bytes_per_second)
        started = time.monotonic()
        sent = 0
        while True:
            if st.stall_after_bytes and not st.stall_done:
                now = time.monotonic()
                if st.throttled_bytes >= st.stall_after_bytes:
                    if st.stall_until == 0.0:
                        st.stall_until = now + st.stall_seconds
                    if now < st.stall_until:
                        if st.stalls_served == 0:
                            st.stalls_served = 1     # count the episode, not the polls
                        # Hold the socket open saying nothing. The connection
                        # stays ESTABLISHED, so the client blocks in read()
                        # rather than seeing an error -- which is the case that
                        # hangs players, and why closing the socket instead is
                        # not a substitute for testing this.
                        time.sleep(0.1)
                        continue
                    st.stall_done = True
            chunk = source.read(8192)
            if not chunk:
                break
            if metered:
                # Sleep for the time this chunk *should* have taken, minus the
                # time already spent. Not a sleep-per-chunk: that would quantise
                # the rate to the chunk size and make 300 KB/s and 3 MB/s differ
                # by 10x less than asked.
                owed = sent / float(st.bytes_per_second)
                drift = owed - (time.monotonic() - started)
                if drift > 0:
                    time.sleep(drift)
            outputfile.write(chunk)
            sent += len(chunk)
            st.throttled_bytes += len(chunk)

    def do_GET(self):       # noqa: N802 -- name fixed by the base class
        st = type(self).state
        st.requests += 1

        if st.drop_every and (st.requests % st.drop_every) == 0:
            # Hang up without a response body. The client sees a truncated read,
            # which is what a connection reset mid-transfer actually looks like.
            st.drops_served += 1
            self.close_connection = True
            try:
                self.wfile.close()
            except OSError:
                pass
            return

        if st.latency_ms:
            time.sleep(st.latency_ms / 1000.0)
        super().do_GET()


def build_clip(ffmpeg, source, outdir, seconds):
    """A clip long enough to be interrupted. Returns its path, or None."""
    path = os.path.join(outdir, "long.mp4")
    result = subprocess.run(
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-stream_loop", "-1",
         "-i", source, "-t", str(seconds), "-c", "copy", path],
        capture_output=True, text=True)
    if result.returncode != 0 or not os.path.exists(path):
        return None, (result.stderr or "").strip()
    return path, None


class Case:
    """One impairment configuration and the end states it is allowed to reach.

    |fired| is the part that matters most and is the reason this file exists in
    this shape. A degraded-network case that does not degrade anything is the
    most dangerous kind of test result: it prints "ok", it goes green in CI,
    and it has demonstrated nothing. So every case declares what MUST have
    actually happened -- at least one held-open response, at least one hung-up
    response, bytes that really passed through the meter -- and a case that
    ends in a defined state without ever applying its damage is reported as
    FAILED.
    """

    def __init__(self, name, latency_ms=0, stall_after_bytes=0,
                 stall_for=0.0,
                 drop_every=0, bytes_per_second=0, allow_error=False,
                 min_stalls=0, min_drops=0, must_throttle=False,
                 gap_note=None):
        self.name = name
        self.latency_ms = latency_ms
        self.stall_after_bytes = stall_after_bytes
        self.stall_for = stall_for
        self.drop_every = drop_every
        self.bytes_per_second = bytes_per_second
        # allow_error=True means "kError is an acceptable answer here". A total
        # outage has no correct answer that is not "give up cleanly", so holding
        # it to kCompleted would be asserting a lie.
        self.allow_error = allow_error
        self.min_stalls = min_stalls
        self.min_drops = min_drops
        self.must_throttle = must_throttle
        # Printed when the case ends in kError rather than kCompleted, so a
        # tolerated-but-unimplemented behaviour is stated rather than absorbed.
        self.gap_note = gap_note


def run_case(headless, url, clip_seconds, case, state):
    """Runs one case end to end. Returns (ok, detail)."""
    state.latency_ms = case.latency_ms
    state.stall_after_bytes = case.stall_after_bytes
    # case.stall_for, not a literal 0. With a zero window the hold lasted no time
    # at all, so the loop fell straight through to stall_done and the case
    # reported "never fired" while the bytes it was watching streamed past. The
    # symptom pointed at the trigger; the bug was one line above it.
    state.stall_seconds = case.stall_for
    state.stall_until = 0.0
    state.stall_done = False
    state.drop_every = case.drop_every
    state.bytes_per_second = case.bytes_per_second
    state.throttled_bytes = 0
    state.requests = 0
    state.stalls_served = 0
    state.drops_served = 0

    budget = clip_seconds + SLACK_SECONDS
    started = time.monotonic()
    try:
        result = subprocess.run(
            [headless, url, "--timeout", str(budget)],
            capture_output=True, text=True, timeout=budget + 30)
    except subprocess.TimeoutExpired:
        return False, ("hung: still running %ds after start -- the timeout the "
                       "process was given had not fired either" % (budget + 30)), []
    elapsed = time.monotonic() - started

    output = (result.stdout or "") + (result.stderr or "")
    # A crash is not an acceptable end state under ANY circumstance here: the
    # whole claim is that a bad network is survivable, and a signal exit or a
    # failed CHECK while finding that out is the bug this file reports.
    for marker in ("Check failed", "AddressSanitizer", "ThreadSanitizer",
                   "FATAL", "Sanitizer"):
        if marker in output:
            return False, ("crashed: %s"
                           % first_line_containing(output, marker)), []
    if result.returncode < 0:
        return False, ("crashed: killed by signal %d" % -result.returncode), []
    if result.returncode not in (0, 1):
        return False, ("unexpected exit code %d" % result.returncode), []
    if result.returncode == 1 and not case.allow_error:
        return False, ("kError on a network that should have been playable: %s"
                       % (first_line_containing(output, "error")
                          or "no error line")), []

    # End state reached. Now the harder question: did anything actually happen?
    notes = ["%d req" % state.requests]
    if state.stalls_served:
        notes.append("%d held open" % state.stalls_served)
    if state.drops_served:
        notes.append("%d hung up" % state.drops_served)
    if state.bytes_per_second:
        notes.append("%d KB metered" % (state.throttled_bytes // 1024))

    def unmet(what):
        return ("the impairment NEVER FIRED after %.1fs [%s] (%s) -- the player "
                "reached an end state without ever meeting the condition this "
                "case exists to create, so it proved nothing"
                % (elapsed, ", ".join(notes), what))

    if state.stalls_served < case.min_stalls:
        return False, unmet("wanted >=%d held-open responses, saw %d"
                            % (case.min_stalls, state.stalls_served)), []
    if state.drops_served < case.min_drops:
        return False, unmet("wanted >=%d hung-up responses, saw %d"
                            % (case.min_drops, state.drops_served)), []
    if case.must_throttle and state.throttled_bytes == 0:
        return False, unmet("no bytes passed through the rate meter"), []

    completed = result.returncode == 0
    verdict = "kCompleted" if completed else "kError (allowed)"
    gaps = [] if completed else [case.gap_note] if case.gap_note else []
    return True, ("%s in %.1fs [%s]" % (verdict, elapsed, ", ".join(notes))), gaps


def first_line_containing(output, needle):
    for line in output.splitlines():
        if needle in line:
            return line.strip()
    return ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--headless", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--testdata", default="tests/testdata")
    parser.add_argument("--clip-seconds", type=int, default=CLIP_SECONDS)
    parser.add_argument("--only", help="run one case by name")
    args = parser.parse_args()

    if not os.path.exists(args.headless):
        print("check_weaknet: headless binary not found: %s" % args.headless)
        return 2
    ffmpeg = shutil.which(args.ffmpeg) or (
        args.ffmpeg if os.path.exists(args.ffmpeg) else None)
    if not ffmpeg:
        print("check_weaknet: ffmpeg CLI not found (%s)" % args.ffmpeg)
        print("  needed to GENERATE the clip; the point of the test is that")
        print("  the media is produced locally, not fetched.")
        return 2
    source = os.path.join(args.testdata, "small_h264_aac_3s.mp4")
    if not os.path.exists(source):
        print("check_weaknet: source media not found: %s" % source)
        return 2

    workdir = tempfile.mkdtemp(prefix="avbase-weaknet-")
    try:
        clip, err = build_clip(ffmpeg, source, workdir, args.clip_seconds)
        if clip is None:
            print("check_weaknet: could not build the %ds clip: %s"
                  % (args.clip_seconds, err))
            return 2

        # A healthy control run first. Without it, a case that fails for an
        # ordinary reason (a bad build, a broken seed) is indistinguishable
        # from a case that failed because the network hurt, and the whole file
        # reports the wrong thing.
        #
        # The clip is ~2.2 MB, so 300 KB/s puts the transfer (~7s) well below
        # the 20s it takes to play: that is the regime where reading and
        # rendering MUST interleave, and therefore the only regime where an
        # outage has anything to interrupt.
        cases = [
            Case("baseline (no impairment)"),
            Case("bandwidth 300 KB/s", bytes_per_second=300 * 1024,
                 must_throttle=True),
            Case("bandwidth 300 KB/s + 400ms latency",
                 bytes_per_second=300 * 1024, latency_ms=400, must_throttle=True),
            # A connection reset mid-transfer. kError is tolerated, but the
            # reason it is tolerated is a real gap and is named as one: see
            # GAP_RETRY below.
            Case("bandwidth 300 KB/s, hang up 1 in 2",
                 bytes_per_second=300 * 1024, drop_every=2,
                 min_drops=1, must_throttle=True, allow_error=True,
                 gap_note=GAP_RETRY),
            # The source goes quiet mid-stream and comes back. Also tolerated
            # as kError, for the same reason.
            # Armed at ~1 MB of an ~2.2 MB clip: the outage therefore lands in
            # the middle of the bulk transfer, every time, regardless of how fast
            # the player gets to it.
            Case("bandwidth 300 KB/s, outage 8s midway",
                 bytes_per_second=300 * 1024,
                 stall_after_bytes=1024 * 1024, stall_for=8.0,
                 min_stalls=1, must_throttle=True, allow_error=True,
                 gap_note=GAP_RETRY),
        ]
        if args.only:
            cases = [c for c in cases if c.name == args.only]
            if not cases:
                print("check_weaknet: no case named %r; known cases:" % args.only)
                for c in cases:
                    print("    %s" % c.name)
                return 2

        state = Impairment()
        port = free_port()
        handler = lambda *a, **kw: ImpairedHandler(   # noqa: E731
            *a, directory=workdir, **kw)
        ImpairedHandler.state = state
        server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        if not wait_for_port(port):
            print("check_weaknet: local httpd did not come up on %d" % port)
            return 2

        url = "http://127.0.0.1:%d/long.mp4" % port
        print("check_weaknet: docs/12 2.5 -- degraded network")
        print("  clip: %ds, budget per case: %ds"
              % (args.clip_seconds, args.clip_seconds + SLACK_SECONDS))
        print("")
        results = []
        gaps = []
        try:
            for case in cases:
                ok, detail, case_gaps = run_case(
                    args.headless, url, args.clip_seconds, case, state)
                print("  %-38s %-7s %s"
                      % (case.name, "ok" if ok else "FAILED", detail))
                results.append((case.name, ok, detail))
                gaps.extend(case_gaps)
        finally:
            server.shutdown()
            server.server_close()

        print("")
        if gaps:
            # Deduped: several cases usually trip the same missing wiring, and
            # the same paragraph printed four times reads like four problems.
            print("  %d tolerated gap(s) worth stating out loud:" % len(set(gaps)))
            for gap in sorted(set(gaps)):
                print("    %s" % gap)
            print("")
        failed = [name for name, ok, _ in results if not ok]
        if failed:
            print("check_weaknet: FAILED -- %d case(s): %s"
                  % (len(failed), ", ".join(failed)))
            print("")
            print("  A case that HANGS is the serious one: a player blocked on a")
            print("  dead socket never comes back, and no correct decoding")
            print("  elsewhere makes that acceptable.")
            print("  A case that reports 'impairment never fired' is the other")
            print("  serious one: the damage did not happen, so the green means")
            print("  nothing was tested.")
            return 1
        print("check_weaknet: all %d case(s) reached a defined end state"
              % len(results))
        return 0
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
