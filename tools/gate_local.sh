#!/usr/bin/env bash
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# tools/gate_local.sh — Local reproduction of avbase's CI gate sequence.
#
# WHY THIS EXISTS
#   This sandbox cannot see GitHub Actions (the proxy drops api.github.com and
#   the HTTPS git protocol). CI is the project's source of truth for "does the
#   tree still hold itself to its own gates", so a CI we cannot read leaves us
#   flying blind after every push. This script runs the *same* gate commands CI
#   runs, on the presets that are feasible on this machine, so a green local run
#   is the same signal CI would give. It is not a substitute for CI on other
#   platforms; it is the part of CI we can actually observe here.
#
# WHAT IT MIRRORS (jobs map to CI jobs in .github/workflows/ci.yml)
#   inv     -> check_invariants.py            (C1/C4/C5/C16/C22-C28, whole tree)
#   fmt     -> check-format                   (clang-format --dry-run --Werror)
#   lint    -> check-cpplint                  (cpplint --repository=., whole tree)
#   tidy    -> check-clang-tidy (core scope)  (clang-tidy 18, no PCH scratch db)
#   apidocs -> apidocs                        (doxygen zero-warning gate + API.md freshness)
#   macos   -> macos-player                   (macos-sdl2: build + freshness + ctest)
#   capi    -> capi                           (build + freshness + ctest + C99 syntax)
#   core    -> full[core]                     (no-ffmpeg: build + freshness + ctest)
#   ffmpeg  -> ffmpeg + e2e-headless          (ffmpeg: build + freshness + ctest)
#   e2e     -> e2e-headless                   (headless plays test media to kCompleted)
#   asan/tsan/ubsan/coverage -> full matrix   (sanitizer/coverage presets)
#
# USAGE
#   tools/gate_local.sh                 # default: inv fmt macos capi
#   tools/gate_local.sh --jobs e2e,core # run a comma list
#   tools/gate_local.sh --all           # every feasible job (slow)
#   tools/gate_local.sh --list          # show job -> CI mapping
#
# EXIT: 0 if every selected job passed, 1 otherwise. A job that is skipped
# because its tool is missing is reported but does not turn the run red.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# --- Tool resolution (same paths as operations.md / ci.yml) -------------------
CF="${CLANG_FORMAT:-$HOME/.workbuddy/binaries/python/envs/default/bin/clang-format}"
CT="${CLANG_TIDY:-$HOME/.workbuddy/binaries/python/envs/tidy18/bin/clang-tidy}"
CP="${CPPLINT:-$HOME/.workbuddy/binaries/python/envs/cpplint/bin/cpplint}"
NINJA="$(command -v ninja || true)"
CMAKE="$(command -v cmake || true)"
FFMPEG_BIN="$(command -v ffmpeg || true)"

if [ -t 1 ]; then
  GREEN=$'\033[32m'; RED=$'\033[31m'; YEL=$'\033[33m'; BOLD=$'\033[1m'; NC=$'\033[0m'
else
  GREEN=; RED=; YEL=; BOLD=; NC=
fi

pass=(); fail=(); skip=()

run_job() {
  local name="$1"; shift
  echo
  echo "${BOLD}================================================================${NC}"
  echo "${BOLD}JOB: $name${NC}"
  echo "${BOLD}================================================================${NC}"
  if "$@"; then
    pass+=("$name"); echo "${GREEN}PASS: $name${NC}"
  else
    local rc=$?
    fail+=("$name"); echo "${RED}FAIL: $name (rc=$rc)${NC}"
  fi
}

need() { [ -n "$1" ] && command -v "$1" >/dev/null 2>&1; }

# --- Generic preset gate: configure -> build all -> invariants -> freshness -> ctest
gate_preset() {
  local p="$1"; local ctest_args="$2"
  if [ -z "$CMAKE" ]; then echo "cmake not found"; return 2; fi
  "$CMAKE" --preset "$p" || return 1
  "$CMAKE" --build "build/$p" || return 1
  # Only run freshness where the build dir is a Ninja tree (the tool reads
  # .ninja_log). The legacy build/ (Unix Makefiles) is not a gate target.
  if [ -f "build/$p/build.ninja" ]; then
    python3 tools/check_binary_freshness.py --root "build/$p" || return 1
  fi
  python3 tools/check_invariants.py --root . || return 1
  # shellcheck disable=SC2086
  ctest $ctest_args
}

# --- Jobs --------------------------------------------------------------------
job_inv() {
  python3 tools/check_invariants.py --root .
}

job_fmt() {
  if [ ! -x "$CF" ]; then echo "clang-format not found at $CF"; return 2; fi
  git ls-files '*.cc' '*.h' 'player/public/c/*.c' \
    | grep -v 'media/filters/legacy/' \
    | grep -v 'player/option_registry.inc' \
    | grep -v 'third_party/' > /tmp/gate_fmt.txt
  echo "check-format: $(wc -l < /tmp/gate_fmt.txt | tr -d ' ') files"
  xargs "$CF" --dry-run --Werror --style=file < /tmp/gate_fmt.txt
}

job_lint() {
  if [ ! -x "$CP" ]; then echo "cpplint not found at $CP"; return 2; fi
  git ls-files 'base/*.cc' 'media/*.cc' 'player/*.cc' 'platform/*.cc' 'tools/*.cc' \
    | grep -v 'legacy/' | xargs "$CP" --repository=.
}

job_apidocs() {
  # Mirrors the CI apidocs job. doxygen is not on every machine (this one
  # keeps it at /tmp/avbase_tools), so DOXYGEN= overrides the lookup; with
  # neither present the job is skipped, per this script's contract.
  if [ -z "${DOXYGEN:-}" ] && ! command -v doxygen >/dev/null 2>&1; then
    echo "doxygen not found; set DOXYGEN=/path/to/doxygen to run this job"
    return 0
  fi
  tools/gen_api_docs.sh && git diff --exit-code docs/API.md
}

job_tidy() {
  if [ ! -x "$CT" ]; then echo "clang-tidy not found at $CT"; return 2; fi
  if [ -z "$CMAKE" ]; then echo "cmake not found"; return 2; fi
  if [ -z "$NINJA" ]; then echo "ninja not found"; return 2; fi
  # Bash 3.2 (macOS) has no mapfile; use a temp file.
  git ls-files 'base/*.cc' 'media/*.cc' 'player/*.cc' 'platform/*.cc' 'tools/*.cc' \
    | grep -v 'legacy/' > /tmp/gate_tidy_scope.txt
  # Configure a scratch DB with PCH off (media/ffmpeg is silent under PCH on
  # this clang-tidy), then run over the non-test scope the way CI does.
  "$CMAKE" --preset ffmpeg -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON \
    -B /tmp/tidy-nopch >/dev/null 2>&1 || { echo "tidy configure failed"; return 1; }
  "$CMAKE" --build /tmp/tidy-nopch >/dev/null 2>&1 || { echo "tidy build failed"; return 1; }
  xargs "$CT" -p /tmp/tidy-nopch -quiet -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" \
    < /tmp/gate_tidy_scope.txt
}

job_macos() {
  gate_preset macos-sdl2 "--test-dir build/macos-sdl2 --output-on-failure" && \
  test -x build/macos-sdl2/bin/play_sdl2 && \
  build/macos-sdl2/bin/play_sdl2 --help >/dev/null 2>&1 || true
}

job_capi() {
  gate_preset capi "--test-dir build/capi --output-on-failure" && \
  ctest --test-dir build/capi -R '^capi_smoke_test$' --output-on-failure && \
  [ -x "$CF" ] && \
  cc -std=c99 -Wall -Wextra -Werror -pedantic -I. -fsyntax-only \
    player/public/c/capi_smoke_test.c
}

job_core() {
  gate_preset no-ffmpeg "--test-dir build/no-ffmpeg --output-on-failure"
}

job_ffmpeg() {
  gate_preset ffmpeg "--test-dir build/ffmpeg --output-on-failure"
}

job_e2e() {
  if [ -z "$FFMPEG_BIN" ]; then echo "ffmpeg CLI not found"; return 2; fi
  if [ -z "$CMAKE" ]; then echo "cmake not found"; return 2; fi
  "$CMAKE" --preset ffmpeg >/dev/null 2>&1 || { echo "e2e configure failed"; return 1; }
  "$CMAKE" --build build/ffmpeg --target headless >/dev/null 2>&1 || { echo "headless build failed"; return 1; }
  local headless=build/ffmpeg/bin/headless
  local fail=0
  for f in small_h264_aac_3s.mp4 audio_only.m4a video_only.mp4 truncated_tail.mp4; do
    if "$headless" "tests/testdata/$f" --timeout 60 >/dev/null 2>&1; then
      echo "ok   $f -> kCompleted"
    else
      echo "FAIL $f did not reach kCompleted"; fail=1
    fi
  done
  if "$headless" tests/testdata/small_h264_aac_3s.mp4 --seek 1.5 --timeout 60 >/dev/null 2>&1; then
    echo "ok   --seek 1.5 -> kCompleted"
  else
    echo "FAIL --seek 1.5"; fail=1
  fi
  local log; log="$(mktemp)"
  if "$headless" tests/testdata/corrupt_header.mp4 --timeout 30 >"$log" 2>&1; then
    echo "FAIL corrupt_header.mp4 unexpectedly reached kCompleted"; fail=1
  elif ! grep -q DecodeFailed "$log"; then
    echo "FAIL corrupt_header.mp4 failed without DecodeFailed detail"; fail=1
  else
    echo "ok   corrupt_header.mp4 -> actionable failure"
  fi
  return $fail
}

job_asan()    { gate_preset asan    "--test-dir build/asan --output-on-failure"; }
job_tsan()    { gate_preset tsan    "--test-dir build/tsan --output-on-failure"; }
job_ubsan()   { gate_preset ubsan   "--test-dir build/ubsan --output-on-failure"; }
job_coverage(){ gate_preset coverage "--test-dir build/coverage --output-on-failure"; }

# --- Dispatch ----------------------------------------------------------------
ALL_JOBS="inv fmt lint tidy apidocs macos capi core ffmpeg e2e asan tsan ubsan coverage"
DEFAULT_JOBS="inv fmt macos capi"

if [ "${1:-}" = "--list" ]; then
  echo "Job -> CI job (see .github/workflows/ci.yml):"
  echo "  inv     check_invariants (whole tree)"
  echo "  fmt     check-format (clang-format --dry-run --Werror)"
  echo "  lint    check-cpplint (cpplint 1.6.1)"
  echo "  tidy    check-clang-tidy (clang-tidy 18, PCH off scratch db)"
  echo "  apidocs apidocs (doxygen zero warnings; needs DOXYGEN= if not on PATH)"
  echo "  macos   macos-player (macos-sdl2)"
  echo "  capi    capi + C99 syntax check"
  echo "  core    full[core no-ffmpeg]"
  echo "  ffmpeg  ffmpeg"
  echo "  e2e     e2e-headless"
  echo "  asan/tsan/ubsan/coverage  full matrix sanitizers/coverage"
  echo "Default: $DEFAULT_JOBS"
  exit 0
fi

JOBS="$DEFAULT_JOBS"
if [ "${1:-}" = "--all" ]; then JOBS="$ALL_JOBS"; fi
if [ "${1:-}" = "--jobs" ] && [ -n "${2:-}" ]; then JOBS="${2//,/ }"; fi

# Map job name -> function; skip unknown.
for j in $JOBS; do
  fn="job_$j"
  if declare -F "$fn" >/dev/null 2>&1; then
    run_job "$j" "$fn"
  else
    echo "${YEL}SKIP: unknown job '$j'${NC}"; skip+=("$j")
  fi
done

echo
echo "${BOLD}=================== GATE SUMMARY ===================${NC}"
echo "${GREEN}PASS (${#pass[@]}): ${pass[*]:-none}${NC}"
echo "${RED}FAIL (${#fail[@]}): ${fail[*]:-none}${NC}"
[ "${#skip[@]}" -gt 0 ] && echo "${YEL}SKIP (${#skip[@]}): ${skip[*]}${NC}"

if [ "${#fail[@]}" -gt 0 ]; then exit 1; fi
exit 0
