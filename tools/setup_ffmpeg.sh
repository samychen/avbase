#!/usr/bin/env bash
# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Builds a pinned FFmpeg from source into a prefix, so that every developer and
# every CI job compiles against the SAME version. Distro packages drift
# (Debian 12 ships 5.1, Ubuntu 24.04 ships 6.1), and avbase's compat layer is
# only meaningful if the version under test is known.
#
# Usage:
#   tools/setup_ffmpeg.sh [version] [prefix] [jobs]
#   export AVBASE_FFMPEG_ROOT=/opt/ffmpeg-7.1.1
#   cmake --preset linux-ffmpeg711
#
# The component list below is deliberately minimal: it covers everything the
# demuxer/decoder tests need and keeps the build near 3 minutes on 2 cores.
# Add components here rather than passing --enable-everything, so build time
# stays bounded.
set -euo pipefail

VERSION="${1:-7.1.1}"
PREFIX="${2:-/opt/ffmpeg-${VERSION}}"
JOBS="${3:-$(nproc)}"
SRC_URL="https://ffmpeg.org/releases/ffmpeg-${VERSION}.tar.xz"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "${WORKDIR}"' EXIT

echo "==> Fetching ${SRC_URL}"
if command -v curl >/dev/null 2>&1; then
  curl -fsSL -o "${WORKDIR}/ffmpeg.tar.xz" "${SRC_URL}"
else
  python3 - "${SRC_URL}" "${WORKDIR}/ffmpeg.tar.xz" <<'PY'
import sys, urllib.request
url, dst = sys.argv[1], sys.argv[2]
req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
with urllib.request.urlopen(req, timeout=300) as r, open(dst, "wb") as f:
    while chunk := r.read(1 << 20):
        f.write(chunk)
PY
fi

echo "==> Extracting"
tar -xf "${WORKDIR}/ffmpeg.tar.xz" -C "${WORKDIR}"
cd "${WORKDIR}/ffmpeg-${VERSION}"

echo "==> Configuring into ${PREFIX}"
./configure \
  --prefix="${PREFIX}" \
  --enable-shared --disable-static \
  --disable-programs --disable-doc --disable-debug --disable-autodetect \
  --disable-x86asm \
  --disable-avfilter --disable-avdevice --disable-postproc \
  --disable-everything \
  --enable-network \
  --enable-protocol=file,http,tcp,httpproxy,crypto \
  --enable-demuxer=mov,matroska,mpegts,flv,mp3,wav,ogg,aac,h264,hevc,avi,mpegps \
  --enable-parser=h264,hevc,aac,mp3,vp9,opus,mpeg4video \
  --enable-decoder=h264,hevc,aac,mp3,vp9,opus,mpeg4 \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata \
  --enable-muxer=null --enable-filter=null \
  --enable-swresample --enable-swscale

# NOTE: https/tls are NOT enabled above because that needs a TLS backend
# (OpenSSL/GnuTLS) which is a separate dependency. Tests use file:// and
# http:// only. Enable --enable-openssl plus --enable-protocol=https,tls here
# when network playback against TLS origins is required.

echo "==> Building with -j${JOBS}"
make -j"${JOBS}"
make install

echo "==> Installed:"
ls "${PREFIX}/lib" | grep -E '\.so\.[0-9]+$' || true
grep -m1 LIBAVCODEC_VERSION_MAJOR "${PREFIX}/include/libavcodec/version_major.h"
echo
echo "Now run:  cmake --preset linux-ffmpeg711   (or -DAVBASE_FFMPEG_ROOT=${PREFIX})"
