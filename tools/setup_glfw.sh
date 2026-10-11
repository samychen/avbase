#!/usr/bin/env bash
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Builds GLFW from source into a repo-local prefix, purely so that the GLFW
# backend (platform/glfw/) and its acceptance example can be COMPILED AND RUN
# on machines whose package manager cannot reach github.  Nothing in avbase
# depends on this: the GLFW backend is optional and skipped (warning, not
# error) when glfw3 is absent.
#
# Why from source at all: `brew install glfw` needs ghcr.io and Homebrew's API
# host, both of which can be unreachable behind a proxy that tunnels only
# some hosts (that was the situation that motivated this script).  The release
# tarball comes over plain HTTPS and often still works.
#
# Usage:
#   tools/setup_glfw.sh [version] [prefix]
#   cmake --preset macos-sdl2 -DAVBASE_ENABLE_GLFW=ON \
#         -DCMAKE_PREFIX_PATH=tools/build/glfw
#
# Default prefix is repo-local tools/build/glfw (gitignored, no sudo), matching
# tools/setup_ffmpeg.sh's convention.
#
# On macOS the static library is built for AVBASE_OSX_DEPLOYMENT_TARGET
# (default 11.0) rather than for the host OS, so linking avbase's examples
# against it does not warn about a newer deployment version.  No effect on Linux.
#
# If github release downloads fail in your network, point at any mirror that
# serves the identical file:
#   AVBASE_GLFW_BASE_URL=https://gh-proxy.com/https://github.com/glfw/glfw/\
# releases/download tools/setup_glfw.sh
# Mirrors are third parties: verify what you get (see the post-download checks
# below, which the script already performs).

set -euo pipefail

VERSION="${1:-3.4}"
PREFIX_IN="${2:-}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${PREFIX_IN:-$ROOT/tools/build/glfw}"
BASE_URL="${AVBASE_GLFW_BASE_URL:-https://github.com/glfw/glfw/releases/download}"

SRC_NAME="glfw-${VERSION}"
TARBALL="${SRC_NAME}.zip"
BUILD_DIR="${PREFIX}/.build-${SRC_NAME}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ -d "$PREFIX" ] && [ -f "${PREFIX}/lib/cmake/glfw3/glfw3Config.cmake" ]; then
  echo "GLFW already installed at ${PREFIX}; remove it to rebuild."
  echo "Configure with: -DCMAKE_PREFIX_PATH=${PREFIX}"
  exit 0
fi

echo ">> downloading ${SRC_NAME}"
curl -fL --retry 3 --retry-all-errors --max-time 300 \
     -o "${WORK}/${TARBALL}" "${BASE_URL}/${VERSION}/${TARBALL}"

# Minimal integrity checks: the payload must actually be GLFW at the version
# asked for. A mirror serving something else fails here rather than silently
# compiling into our tree.
unzip -q -o "${WORK}/${TARBALL}" -d "$WORK"
if [ ! -f "${WORK}/${SRC_NAME}/CMakeLists.txt" ] ||
   [ ! -f "${WORK}/${SRC_NAME}/include/GLFW/glfw3.h" ]; then
  echo "!! ${TARBALL} does not look like a GLFW source tree; refusing to build"
  exit 1
fi
FOUND_VERSION="$(grep -m1 -oE 'project\(GLFW VERSION [0-9.]+' \
                 "${WORK}/${SRC_NAME}/CMakeLists.txt" | awk '{print $3}')"
echo ">> source says version ${FOUND_VERSION}"
case "$FOUND_VERSION" in
  "${VERSION}"|"${VERSION}".0) ;;
  *) echo "!! want ${VERSION}, source is ${FOUND_VERSION}; refusing to build"
     exit 1 ;;
esac

echo ">> building into ${PREFIX}"
mkdir -p "$PREFIX"
# Pin the macOS deployment target.  Left to itself CMake builds GLFW for the host
# OS version, and then every avbase link prints a wall of
#   ld: warning: object file (libglfw3.a[...]) was built for newer 'macOS'
#   version (15.8) than being linked (15.7)
# because the host is newer than the SDK's default target.  Ignored by CMake on
# non-Apple platforms, so this stays correct for the Linux users of this script.
cmake -S "${WORK}/${SRC_NAME}" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DGLFW_BUILD_EXAMPLES=OFF \
      -DGLFW_BUILD_TESTS=OFF \
      -DGLFW_BUILD_DOCS=OFF \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="${AVBASE_OSX_DEPLOYMENT_TARGET:-11.0}" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$BUILD_DIR"
cmake --install "$BUILD_DIR"
rm -rf "$BUILD_DIR"

cat <<EOF

Installed GLFW ${FOUND_VERSION} into ${PREFIX}

Build the backend with:

  cmake --preset macos-sdl2 \\
        -DAVBASE_ENABLE_GLFW=ON \\
        -DCMAKE_PREFIX_PATH=${PREFIX}
  cmake --build build/macos-sdl2

Then:  ./build/macos-sdl2/bin/play_glfw --url tests/testdata/small_h264_aac_3s.mp4

Note the CMake package exports its imported target as \`glfw\`, even though the
library file is named libglfw3.a.

EGL is a different story: macOS ships no EGL development library, so
examples/play_egl.cc cannot be built there at all. Use a Linux box with Mesa.
EOF
