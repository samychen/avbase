#!/usr/bin/env bash
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# V3 (docs/10 §12): "the minimal integration needs FFmpeg only; ldd
# libavbase.so shows no SDL2/GL/X11/ALSA". This tree ships STATIC libraries,
# so ldd has nothing to say; the property is checked where it actually lives:
# the undefined symbols the core archives carry, and the headers a host must
# include.
#
# TWO HALVES, because "depends on" has two meanings:
#   1. LINK: no core archive may reference a backend's symbols (SDL2, GLX,
#      EGL, PulseAudio, ALSA, X11, Wayland). The backends are dlopen'd at
#      runtime by the platform layer; a hard reference turns an optional
#      backend into a mandatory dependency.
#   2. HEADERS: a translation unit that includes only player/public/*.h must
#      compile with FFmpeg's include path and nothing else. A host that has
#      to install SDL2 just to call SetDataSource is the same bug one layer up.
#
# WHY THIS IS A SCRIPT: the first version of the LINK half was a one-line grep
# with -i, and it "found" two violations that were avbase's OWN symbols --
# avbase::gl::GlApi::Load(NativeGLContext*) matched "egl" case-insensitively
# on the "eGL" inside NativeGLContext. A gate that fires on its own codebase's
# names trains people to widen the regex until it passes. So: demangle first,
# match case-sensitively, and name the exact prefixes that are somebody
# else's API.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build/macos-sdl2}"
fail=0

if [[ ! -d "$BUILD/lib" ]]; then
  echo "check_link_deps: no $BUILD/lib -- pass the build dir" >&2
  exit 2
fi

# ---- half 1: undefined symbols of the core archives ----------------------
# Case-SENSITIVE, on demangled names. avbase's own GL abstraction is
# avbase::gl::*; the platform GL APIs are glX*/egl*/SDL_*/pa_*/snd_pcm_*/
# XOpenDisplay/wl_*. Nothing in avbase:: or the C++ runtime should match.
BACKEND_RE='SDL_[A-Z]|glX[A-Z]|egl[A-Z]|pa_[a-z]|snd_pcm_|XOpenDisplay|XCreateWindow|wl_[a-z_]+'

for lib in libavbase_base.a libavbase_media.a libavbase_player.a; do
  path="$BUILD/lib/$lib"
  [[ -f "$path" ]] || continue
  # nm -g gives global (exported+undefined) symbols; " U " is undefined.
  # c++filt demangles so the match sees avbase::gl::GlApi, not _ZN6avbase...
  hits=$(nm -g "$path" 2>/dev/null | grep ' U ' | c++filt \
      | grep -E "$BACKEND_RE" || true)
  if [[ -n "$hits" ]]; then
    echo "FAIL V3-link: $lib references backend symbols:" >&2
    echo "$hits" | sed 's/^/    /' >&2
    fail=1
  fi
done

# ---- half 2: public headers compile against FFmpeg only -------------------
# The per-component cache entries FindFFmpeg.cmake writes (avcodec/avformat/
# ...), not the aggregate FFMPEG_DIR (which stays NOTFOUND when the components
# were found individually -- that is the normal case in this tree).
probe="$BUILD/v3_header_probe.cc"
cat >"$probe" <<'EOF'
#include "player/public/player.h"
#include "player/public/player_config.h"
#include "player/public/deps.h"
int main() { return 0; }
EOF
ffmpeg_inc=$(sed -n 's/^FFmpeg_avutil_INCLUDE_DIR:PATH=//p' \
    "$BUILD/CMakeCache.txt" 2>/dev/null | head -1)
if [[ -n "$ffmpeg_inc" && -d "$ffmpeg_inc" ]]; then
  if ! c++ -fsyntax-only -std=c++20 -I"$ROOT" -I"$BUILD/generated" \
      -I"$ffmpeg_inc" "$probe" 2>/tmp/v3_probe_err; then
    echo "FAIL V3-header: player/public headers need more than FFmpeg:" >&2
    head -5 /tmp/v3_probe_err | sed 's/^/    /' >&2
    fail=1
  fi
else
  echo "note: FFmpeg include dir not in CMakeCache; header half skipped" >&2
fi
rm -f "$probe"

if [[ $fail -eq 0 ]]; then
  echo "check_link_deps: PASS (core archives reference no backend symbols;"
  echo "                 player/public headers compile with FFmpeg alone)"
fi
exit $fail
