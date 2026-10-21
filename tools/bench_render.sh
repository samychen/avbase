#!/usr/bin/env bash
#
# bench_render.sh - end-to-end measurement of avbase's GL render path.
#
# Plays synthetic clips through play_glfw with --stats and prints the four
# stages the sink measures (upload / compose / blit / present), the output frame
# rate and the upload path. The point is one command that answers "what does a
# frame of my player cost", rather than a number someone remembered from a
# profile run.
#
# Usage:
#   tools/bench_render.sh [build-dir] [seconds-per-clip] [canvas WxH]
#
# Defaults: build/macos-sdl2-werror, 12 seconds per clip, no fixed canvas (the
# composition surface follows the window).
#
# Set SCENE=pip or SCENE=fade to measure the multi-source path instead of the
# single-source one; see examples/scene_demo.h. An environment variable rather
# than a positional because passing a scene without a canvas would otherwise
# mean an empty argument.
#
# Set SECOND=<url> to feed the second layer from a SECOND DECODER instead of
# the delayed copy of the same stream that SCENE alone uses. docs/14 stage 3
# made that second decoder part of the same pipeline: it costs an extra decode,
# not just an extra upload, which is exactly why it is the path worth
# measuring. (Before stage 3 this needed a second whole Player whose output
# arrived through a mailbox; the numbers mean the same thing either way, but a
# regression now shows up in one pipeline's counters rather than being split
# across two.)
#
# Requires: ffmpeg on PATH (to build the clips), python3, and a build that
# includes play_glfw (AVBASE_ENABLE_GLFW=ON with glfw3 found) as well as a
# display to present to -- this measures the real swap path, so a headless run
# would report compose cost and no present cost.
#
# Before measuring anything this script runs tools/check_binary_freshness.py
# against the build dir and refuses to continue when the tree is stale. The
# failure it prevents is silent: a binary linked before a fix reports the old
# numbers with full confidence, and this script's medians over a steady tail
# make them read like a clean measurement rather than a stale one -- which is
# exactly how a six-day-old tree once produced a "render path starves after 40
# frames" report for a bug that had been fixed three days earlier. A Ninja
# tree that cannot be judged at all is refused too, with the checker's own
# reason; a non-Ninja tree cannot be checked, so that case warns and proceeds,
# because the script accepts an arbitrary build dir.
#
# HOW TO READ THE NUMBERS (learned the hard way):
#
#   * The clip must outlast the start-up. A window takes roughly a second to
#     map and settle, and while it does the sink is starved, the swap is not
#     vsync-throttled yet, and the frame rate reads low. Averaging that window
#     into the result is how a 60 fps pipeline gets reported as 53, which is
#     why every figure below is a median over the STEADY TAIL of the samples
#     rather than a mean over all of them, and why the default clip is 12 s
#     rather than 3. Short clips make this tool lie.
#   * EVERY figure is that median, including the four stage costs. Reading the
#     stages from the final sample instead -- which this script did -- makes
#     the whole row subject to whatever the machine was doing in the last
#     second: a 2-layer run once reported 0.6 ms of upload where the run's own
#     samples said 1.5 ms.
#   * A loaded machine moves the same numbers. Before blaming a change, check
#     that the figure swings back when the load does; two backends agreeing on
#     the same wrong number is the machine talking, not the code.
#   * `max` is cumulative since the process started, so on a fresh run it is
#     the start-up spike (the compositor throttling a window that is not on
#     screen yet), not a steady-state worst case. Read it as "did anything
#     stall at all", never as the tail latency.
#
# Stage meanings (see media/base/render_perf_stats.h):
#   upload   plane pixels -> GPU.
#   compose  clear + the draw passes, into the off-screen canvas.
#   blit     the composite -> window pass, scaled to the drawable.
#   present  SwapBuffers, which blocks on vsync when the interval is on. A
#            present close to the refresh period is expected, not a problem.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build/macos-sdl2-werror}"
SECONDS_PER_CLIP="${2:-12}"
CANVAS="${3:-}"
SCENE="${SCENE:-}"
SECOND="${SECOND:-}"
PLAYER="$BUILD_DIR/bin/play_glfw"
CLIP_DIR="${TMPDIR:-/tmp}/avbase-bench"

if [ ! -x "$PLAYER" ]; then
  echo "no play_glfw at $PLAYER" >&2
  echo "build it first: cmake --build $BUILD_DIR --target play_glfw" >&2
  exit 1
fi
# Staleness gate. Checked before a single clip is generated: the numbers this
# script prints are only about the code that is in the binary, and a stale
# binary answers about code that no longer exists. The scope is play_glfw
# specifically (--binary) because that is the only binary this script runs; a
# stale unittest in the same tree is none of this measurement's business.
# `if !` rather than a bare call because `set -e` would otherwise abort
# without the explanation below.
if [ -f "$BUILD_DIR/build.ninja" ]; then
  if ! freshness="$(python3 "$ROOT/tools/check_binary_freshness.py" \
      --root "$BUILD_DIR" --binary play_glfw 2>&1)"; then
    echo "refusing to measure: play_glfw in $BUILD_DIR does not describe" \
      "the current sources" >&2
    echo "$freshness" >&2
    echo "rebuild first: cmake --build $BUILD_DIR --target play_glfw" >&2
    exit 1
  fi
else
  echo "note: $BUILD_DIR is not a Ninja tree; freshness not checked" >&2
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg is not on PATH; it is what builds the clips" >&2
  exit 1
fi

# Passing a canvas exercises the scaling path, which is where blit_ms is not
# just the cost of the round trip but of resampling too.
PLAYER_ARGS=()
if [ -n "$CANVAS" ]; then
  PLAYER_ARGS+=(--canvas "$CANVAS")
fi
# Same expansion rule as below, for the same reason.
if [ -n "$SCENE" ]; then
  PLAYER_ARGS+=(--scene "$SCENE")
fi
if [ -n "$SECOND" ]; then
  PLAYER_ARGS+=(--second "$SECOND")
fi

mkdir -p "$CLIP_DIR"

# --second needs --scene: the second layer IS a scene layer, so without a mode
# the host has nothing to place it in and the flag would be silently ignored --
# which is the failure mode this check exists to prevent.
if [ -n "$SECOND" ] && [ -z "$SCENE" ]; then
  echo "SECOND needs SCENE (the second layer is a scene layer)" >&2
  exit 1
fi

# name:size:rate:bitrate. The spread is deliberate: 720p catches the CPU-bound
# small case, 1080p60 is the common target, 4K is where upload stops being
# free.
CLIPS=(
  "720p60:1280x720:60:4M"
  "1080p30:1920x1080:30:8M"
  "1080p60:1920x1080:60:8M"
  "2160p30:3840x2160:30:20M"
)

echo "build:   $BUILD_DIR (freshness checked)"
echo "clips:   $CLIP_DIR (${SECONDS_PER_CLIP}s each)"
echo "canvas:  ${CANVAS:-(follows the window)}"
echo "scene:   ${SCENE:-(single source)}"
echo "second:  ${SECOND:-(same stream, delayed)}"
echo

printf '%-9s %8s %8s %9s %9s %9s %9s %9s  %s\n' \
  "clip" "fps" "frames" "upload" "compose" "blit" "present" "max*" "path"
printf '%-9s %8s %8s %9s %9s %9s %9s %9s  %s\n' \
  "---------" "--------" "--------" "---------" "---------" "---------" \
  "---------" "---------" "----"

for spec in "${CLIPS[@]}"; do
  IFS=':' read -r name size rate bitrate <<<"$spec"
  # The requested length is part of the file name so that changing it
  # regenerates instead of silently reusing a clip that is now too short to
  # measure -- a stale 3 s clip is exactly what makes this tool under-report.
  clip="$CLIP_DIR/${name}-${SECONDS_PER_CLIP}s.mp4"
  if [ ! -f "$clip" ]; then
    ffmpeg -y -loglevel error -f lavfi \
      -i "testsrc2=size=$size:rate=$rate" -t "$SECONDS_PER_CLIP" \
      -pix_fmt yuv420p -c:v libx264 -preset veryfast -b:v "$bitrate" "$clip"
  fi
  # Play past the end of the clip so the last samples are all steady state.
  #
  # The argument expansion is deliberately not the plain "${PLAYER_ARGS[@]}":
  # under `set -u` bash 3.2 -- what macOS ships and what /usr/bin/env finds --
  # treats an empty array as an unbound variable and aborts. The +"..." form
  # expands to nothing instead and is correct on 4.x as well. This is the
  # difference between the script running with no canvas and only ever running
  # with one, which is how the empty-array case went unnoticed.
  "$PLAYER" --url "$clip" --max-seconds "$((SECONDS_PER_CLIP + 4))" --stats \
    ${PLAYER_ARGS[@]+"${PLAYER_ARGS[@]}"} 2>/dev/null \
    | python3 -c '
import json, statistics, sys

# Filter here rather than with grep: a grep that matches nothing exits 1, and
# under `set -e -o pipefail` that kills the script before the diagnostic below
# can explain why.
samples = []
for line in sys.stdin:
    line = line.strip()
    if line.startswith("stats "):
        try:
            samples.append(json.loads(line.split(" ", 1)[1]))
        except ValueError:
            pass

name = sys.argv[1]
if not samples:
    print("%-9s  (no stats -- did the window open?)" % name)
    raise SystemExit

# Start-up samples carry a rate of 0 (no frame out yet) or a rate depressed by
# the window still mapping. Dropping them is the whole point: folding them in
# is how a 60 fps pipeline gets reported as 53.
started = [s for s in samples if s["video_output_fps"] > 0.0]
if not started:
    print("%-9s  (never reached a steady rate)" % name)
    raise SystemExit

# Median of the tail, not of everything: one long pause early on would drag a
# mean, and the median of a short clip is just the start-up value.
tail = started[-5:]
last = tail[-1]
r = last["render"]


def med(key):
    return statistics.median(s["render"][key] for s in tail)


note = "" if len(started) >= 3 else "  (!%d steady samples)" % len(started)
print("%-9s %8.2f %8d %9.3f %9.3f %9.3f %9.3f %9.1f  %s%s" % (
    name, statistics.median(s["video_output_fps"] for s in tail),
    last["frames_presented"], med("upload_ms"), med("compose_ms"),
    med("blit_ms"), med("present_ms"), r["max_present_ms"], r["upload_path"],
    note))
' "$name"
done

echo
echo "fps and every stage cost are medians over the steady tail of the samples"
echo "-- the start-up window is dropped, not averaged in."
echo "present_ms near the refresh period means the swap is vsync-throttled,"
echo "which is the intent. blit_ms is the off-screen canvas round trip; it is 0"
echo "only on a path that composes nothing (the SDL_Renderer back end draws"
echo "straight to the window)."
echo "max* is the worst present since the process started, so on a fresh run it"
echo "is the start-up spike rather than a steady-state worst case."
echo "upload_ms covers every layer the frame reached, so a second layer adds a"
echo "second upload to the same frame's figure. How much that costs depends on"
echo "the route: a hardware import re-points a pointer (measured here at a few"
echo "hundredths of a millisecond at 1080p60 -- below this machine's run-to-run"
echo "noise), a CPU copy moves a whole frame of pixels and really does read as"
echo "roughly double. Read upload_path first: that is what tells them apart."
