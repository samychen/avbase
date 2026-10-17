#!/usr/bin/env bash
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Weak-network regression for the live streaming protocols (docs/12 2.5).
#
# RTMP/RTSP/SRT must degrade gracefully -- not stall -- when the link is lossy,
# delayed or reorders packets. This harness imposes a controlled impairment with
# `tc netem` on the loopback device, then drives tools/check_live_protocols.py
# and asserts the player still advances the media clock past the threshold.
#
# REQUIREMENTS (environment gaps, not script bugs):
#   * Linux with `tc` (iproute2) -- macOS has no tc; use pfctl/dnctl there.
#   * root -- netem writes qdiscs, which is privileged.
# The script only *scripts* the regression; the real 48h/matrix runs happen on
# CI (see docs/12 2.5 / 4.4). It always tears the qdisc down, even on error.
#
# PROFILES (passed to `tc qdisc ... netem`):
#   mild      delay 50ms, loss 0.5%
#   moderate  delay 150ms, loss 2%, reorder 25% 50%
#   severe    delay 400ms, loss 5%, reorder 50% 25%
#
# EXIT CODES mirror check_live_protocols.py, plus:
#   2  environment gap (not root, no tc, no target protocol server)
# and the underlying player/behaviour failure still surfaces as exit 1.

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
CHECKER="$HERE/check_live_protocols.py"

PROTOCOL="srt"
PROFILE="moderate"
HEADLESS=""
MEDIAMTX=""
FFMPEG="ffmpeg"
TESTDATA="tests/testdata"
PLAY_BUDGET=15
MEDIA_THRESHOLD=2.0

usage() {
  cat <<EOF
usage: $0 [--protocol srt|rtsp|rtmp] [--profile mild|moderate|severe]
         [--headless PATH] [--mediamtx PATH] [--ffmpeg PATH]
         [--testdata DIR] [--play-budget S] [--media-threshold S]

Imposes a tc netem impairment on lo and runs check_live_protocols.py.
Needs Linux + root. See header comment for profiles and exit codes.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --protocol) PROTOCOL="$2"; shift 2;;
    --profile)  PROFILE="$2"; shift 2;;
    --headless) HEADLESS="$2"; shift 2;;
    --mediamtx) MEDIAMTX="$2"; shift 2;;
    --ffmpeg)   FFMPEG="$2"; shift 2;;
    --testdata) TESTDATA="$2"; shift 2;;
    --play-budget) PLAY_BUDGET="$2"; shift 2;;
    --media-threshold) MEDIA_THRESHOLD="$2"; shift 2;;
    -h|--help) usage; exit 0;;
    *) echo "unknown arg: $1" >&2; usage >&2; exit 2;;
  esac
done

# ---- environment gates -----------------------------------------------------
if ! command -v tc >/dev/null 2>&1; then
  echo "check_weaknet: 'tc' (iproute2) not found -- macOS/Windows need a different shaper" >&2
  exit 2
fi
if [ "$(id -u)" -ne 0 ]; then
  echo "check_weaknet: must run as root (tc netem writes qdiscs)" >&2
  exit 2
fi

case "$PROFILE" in
  mild)      NETEM="delay 50ms loss 0.5%";;
  moderate)  NETEM="delay 150ms loss 2% reorder 25% 50%";;
  severe)    NETEM="delay 400ms loss 5% reorder 50% 25%";;
  *) echo "check_weaknet: unknown profile: $PROFILE" >&2; exit 2;;
esac
case "$PROTOCOL" in
  rtmp|rtsp|srt) ;;
  *) echo "check_weaknet: unknown protocol: $PROTOCOL" >&2; exit 2;;
esac

# ---- teardown trap ---------------------------------------------------------
IFACE="lo"
teardown() {
  tc qdisc del dev "$IFACE" root 2>/dev/null || true
}
trap teardown EXIT INT TERM

# ---- impose impairment -----------------------------------------------------
echo "check_weaknet: applying netem on $IFACE: $NETEM"
if ! tc qdisc add dev "$IFACE" root netem $NETEM; then
  echo "check_weaknet: failed to add netem qdisc" >&2
  exit 2
fi

# ---- drive the player ------------------------------------------------------
echo "check_weaknet: protocol=$PROTOCOL profile=$PROFILE"
ARGS=(--headless "${HEADLESS:?set --headless}" --ffmpeg "$FFMPEG" \
      --protocols "$PROTOCOL" --play-budget "$PLAY_BUDGET" \
      --media-threshold "$MEDIA_THRESHOLD" --testdata "$TESTDATA")
if [ -n "$MEDIAMTX" ]; then
  ARGS+=(--mediamtx "$MEDIAMTX")
fi

python3 "$CHECKER" "${ARGS[@]}"
rc=$?

if [ "$rc" -eq 0 ]; then
  echo "check_weaknet: PASS -- $PROTOCOL advanced the clock under [$PROFILE]"
elif [ "$rc" -eq 1 ]; then
  echo "check_weaknet: FAIL -- $PROTOCOL stalled/errored under [$PROFILE]" >&2
else
  echo "check_weaknet: environment gap (rc=$rc) under [$PROFILE]" >&2
fi
exit "$rc"
