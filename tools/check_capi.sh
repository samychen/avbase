#!/bin/sh
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Builds and runs the C ABI smoke test (docs/08 M15).
#
# The point of this script is the COMPILE STEP: tools/check_capi.c is compiled
# by the C compiler, which is the only thing that can prove avbase_c.h is valid
# C. A C++ compile of the same header would pass while proving nothing, since
# every C++-ism in it would be silently accepted.
#
# The link uses the C++ driver on purpose -- the engine underneath is C++ and
# needs libc++. The .o stays genuine C, so the boundary under test is real.
#
# USAGE
#   tools/check_capi.sh [source.mp4] [build_dir]
#
# EXIT CODES
#   0  every check passed
#   1  a check failed (see the output)
#   2  a prerequisite is missing (build dir, target not built, no C compiler)

set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
source_media=${1:-$root/tests/testdata/small_h264_aac_3s.mp4}
build=${2:-$root/build/ffmpeg}

if [ ! -f "$source_media" ]; then
  echo "check_capi: source media not found: $source_media" >&2
  exit 2
fi
if [ ! -d "$build" ]; then
  echo "check_capi: build dir not found: $build" >&2
  exit 2
fi

cc_bin=${CC:-cc}
cxx_bin=${CXX:-c++}
if ! command -v "$cc_bin" >/dev/null 2>&1; then
  echo "check_capi: no C compiler (set CC)" >&2
  exit 2
fi

# The C ABI target must already be built: configuring with
# -DAVBASE_ENABLE_CAPI=ON is a deliberate step, not something this script
# forces, because the option is OFF by default on purpose.
capi_lib=$build/lib/libavbase_cabi.a
if [ ! -f "$capi_lib" ]; then
  echo "check_capi: $capi_lib missing." >&2
  echo "  Configure with -DAVBASE_ENABLE_CAPI=ON and build target avbase_cabi." >&2
  exit 2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "check_capi: compiling tools/check_capi.c as C (-std=c99 -Werror)"
"$cc_bin" -std=c99 -Wall -Wextra -Werror \
  -I"$root" -c "$root/tools/check_capi.c" -o "$tmp/check_capi.o"

echo "check_capi: linking"
# The library list mirrors what the engine's own binaries link; rpath points at
# the pinned FFmpeg dylibs so the built binary runs without extra env setup.
"$cxx_bin" -mmacosx-version-min=15.7 "$tmp/check_capi.o" -o "$tmp/check_capi" \
  -Wl,-rpath,"$root/tools/build/lib" \
  "$capi_lib" \
  "$build/lib/libavbase_player.a" \
  "$build/lib/libavbase_ffmpeg.a" \
  "$build/lib/libavbase_media.a" \
  "$build/lib/libavbase_base.a" \
  "$build/lib/libavbase_gl.a" \
  "$root"/tools/build/lib/libavformat.dylib \
  "$root"/tools/build/lib/libavcodec.dylib \
  "$root"/tools/build/lib/libavutil.dylib \
  "$root"/tools/build/lib/libswscale.dylib \
  "$root"/tools/build/lib/libswresample.dylib \
  "$root"/tools/build/lib/libavfilter.dylib \
  "$root"/tools/build/lib/libyuv.dylib \
  "$root"/tools/build/lib/libsrt.1.5.3.dylib 2>&1 | grep -v '^ld: warning' || true

if [ ! -x "$tmp/check_capi" ]; then
  echo "check_capi: link produced no binary" >&2
  exit 2
fi

echo "check_capi: running"
# The exit status has to come from check_capi itself, not from grep, so the run
# is not through a pipe: /bin/sh has no PIPESTATUS, and `set -e` would abort
# before the status could be reported.
set +e
"$tmp/check_capi" "$source_media" >"$tmp/run.log" 2>&1
status=$?
set -e
grep -vE '^\[(INFO|WARN|ERROR)' "$tmp/run.log" || true
exit "$status"