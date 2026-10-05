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
# The default component list is deliberately minimal: it covers everything the
# demuxer/decoder tests need and keeps the build near 3 minutes on 2 cores.
# Add components here rather than passing --enable-everything, so build time
# stays bounded.
#
# Optional third-party dependencies (MediaComponent-style), default OFF so the
# default path stays fast. Everything selected is built from source into the
# SAME prefix as FFmpeg, and the matching FFmpeg configure flags are appended
# automatically:
#   AVBASE_FFMPEG_DEPS="openssl x264 fdk-aac opus librtmp mbedtls srt"
#     openssl   TLS backend + https protocol (preferred over mbedtls)
#     mbedtls   alternative TLS backend; auto-selected for srt when openssl
#               is not in the list
#     x264      libx264 encoder (adds --enable-gpl + mp4/matroska/adts muxers)
#     fdk-aac   libfdk_aac encoder (adds --enable-nonfree + the muxers above)
#     opus      libopus encoder/decoder
#     librtmp   rtmp/rtmps/... protocols (auto-pulls openssl; replaces the
#               native rtmp client)
#     srt       SRT protocol (needs openssl or mbedtls; auto-adds mbedtls)
#
# Caching and link mode:
#   AVBASE_FFMPEG_SRC_DIR  persistent scratch dir for the tarball + extracted
#                          source + per-mode build trees (default tools/deps).
#                          Second runs skip the download and rebuild incrementally.
#   AVBASE_FFMPEG_LINK     shared (default) | static | both. "static" adds
#                          --enable-pic so the .a is safe to link into shared
#                          objects (avbase sets CMAKE_POSITION_INDEPENDENT_CODE).
#   AVBASE_FFMPEG_SHA256   expected checksum, overriding the built-in pin.
set -euo pipefail

VERSION="${1:-7.1.1}"
PREFIX="${2:-/opt/ffmpeg-${VERSION}}"
LINK="${AVBASE_FFMPEG_LINK:-shared}"
SRC_URL="https://ffmpeg.org/releases/ffmpeg-${VERSION}.tar.xz"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="${AVBASE_FFMPEG_SRC_DIR:-${SCRIPT_DIR}/deps}"
DOWNLOADS="${SRC_DIR}/downloads"
TARBALL="${DOWNLOADS}/ffmpeg-${VERSION}.tar.xz"

case "${LINK}" in
  shared|static|both) ;;
  *) echo "error: AVBASE_FFMPEG_LINK must be shared, static or both (got '${LINK}')" >&2
     exit 1 ;;
esac

if command -v nproc >/dev/null 2>&1; then
  DEFAULT_JOBS="$(nproc)"
else
  DEFAULT_JOBS="$(getconf _NPROCESSORS_ONLN)"
fi
JOBS="${3:-${DEFAULT_JOBS}}"

# ---------------------------------------------------------------- checksums --
# Pinned checksums, so a truncated/corrupted/mirror-tampered tarball fails
# here instead of half an hour later in make. Extend per released version.
# Plain `case` rather than `declare -A`: macOS still ships bash 3.2, which
# has no associative arrays and evaluates the subscript arithmetically.
pinned_sha256() {
  case "$1" in
    7.1.1) echo "733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1" ;;
    *)     return 1 ;;
  esac
}
EXPECTED_SHA256="${AVBASE_FFMPEG_SHA256:-}"
if [ -z "${EXPECTED_SHA256}" ]; then
  EXPECTED_SHA256="$(pinned_sha256 "${VERSION}" 2>/dev/null || true)"
fi

# ------------------------------------------------------------------ helpers --
fetch_and_extract() {  # <url> <tarball-name> <expected-sha> <dest-dir>
  local url="$1" name="$2" sha="$3" dest="$4"
  local tarball="${DOWNLOADS}/${name}"
  mkdir -p "${DOWNLOADS}"
  if [ -f "${tarball}" ]; then
    echo "    reusing cached ${tarball}"
  else
    if command -v curl >/dev/null 2>&1; then
      curl -fSL --retry 3 -o "${tarball}.part" "${url}"
    else
      python3 - "${url}" "${tarball}.part" <<'PY'
import sys, urllib.request
url, dst = sys.argv[1], sys.argv[2]
req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
with urllib.request.urlopen(req, timeout=300) as r, open(dst, "wb") as f:
    while chunk := r.read(1 << 20):
        f.write(chunk)
PY
    fi
    mv "${tarball}.part" "${tarball}"
  fi
  if [ -n "${sha}" ]; then
    echo "${sha}  ${tarball}" | shasum -a 256 -c - >/dev/null
    echo "    sha256 ok: ${name}"
  else
    echo "    WARNING: no pinned sha256 for ${name}; got $(shasum -a 256 "${tarball}" | awk '{print $1}')"
  fi
  rm -rf "${dest}"
  mkdir -p "${dest}"
  tar -xf "${tarball}" -C "${dest}" --strip-components=1
}

# ------------------------------------------------------- optional deps layer --
# Sources are pinned by GIT COMMIT (not tarball checksum): tag -> commit is
# verified after clone, which is a stronger integrity guarantee than a file
# hash and works even where GitHub's tarball endpoints are unreachable.
# x264 and librtmp have no release tags: x264 pins a `stable` branch snapshot
# (re-pin if the branch moves), librtmp pins the master HEAD at authoring time.
dep_git_url() {
  case "$1" in
    openssl)  echo "https://github.com/openssl/openssl.git" ;;
    x264)     echo "https://code.videolan.org/videolan/x264.git" ;;
    fdk-aac)  echo "https://github.com/mstorsjo/fdk-aac.git" ;;
    opus)     echo "https://github.com/xiph/opus.git" ;;
    librtmp)  echo "https://git.ffmpeg.org/rtmpdump.git" ;;
    mbedtls)  echo "https://github.com/Mbed-TLS/mbedtls.git" ;;
    srt)      echo "https://github.com/Haivision/srt.git" ;;
  esac
}
dep_tag() {
  case "$1" in
    openssl)  echo "openssl-3.0.15" ;;
    x264)     echo "stable" ;;
    fdk-aac)  echo "v2.0.2" ;;
    opus)     echo "v1.5.2" ;;
    librtmp)  echo "" ;;  # no tags; HEAD snapshot
    mbedtls)  echo "mbedtls-3.5.0" ;;
    srt)      echo "v1.5.3" ;;
  esac
}
dep_commit() {
  case "$1" in
    openssl)  echo "c523121f902fde2929909dc7f76b13ceb4961efe" ;;
    x264)     echo "b35605ace3ddf7c1a5d67a2eb553f034aef41d55" ;;
    fdk-aac)  echo "801f67f671929311e0c9952c5f92d6e147c7b003" ;;
    opus)     echo "ddbe48383984d56acd9e1ab6a090c54ca6b735a6" ;;
    librtmp)  echo "138fdb258d9fc26f1843fd1b891180416c9dc575" ;;
    mbedtls)  echo "1ec69067fa1351427f904362c1221b31538c8b57" ;;
    srt)      echo "09f35c0f1743e23f514cb41444504a7faeacf89e" ;;
  esac
}

# Normalize + validate AVBASE_FFMPEG_DEPS into dependency-safe order, with the
# implicit requirements pulled in explicitly (a notice beats a mystery link
# error 20 minutes in).
SUPPORTED_DEPS=" openssl mbedtls x264 fdk-aac opus librtmp srt "
REQUESTED_DEPS="$(echo "${AVBASE_FFMPEG_DEPS:-}" | tr ',+' '  ')"
DEPS=""
for dep in ${REQUESTED_DEPS}; do
  case "${SUPPORTED_DEPS}" in
    *" ${dep} "*) ;;
    *) echo "error: unsupported AVBASE_FFMPEG_DEPS entry '${dep}' (supported: openssl mbedtls x264 fdk-aac opus librtmp srt)" >&2
       exit 1 ;;
  esac
  case " ${DEPS} " in
    *" ${dep} "*) ;;
    *) DEPS="${DEPS} ${dep}" ;;
  esac
done
if [ -n "${DEPS}" ]; then
  case " ${DEPS} " in
    *" librtmp "*)
      case " ${DEPS} " in
        *" openssl "*) ;;
        *) DEPS=" openssl${DEPS}"; echo "==> note: librtmp needs OpenSSL; added automatically" ;;
      esac ;;
  esac
  case " ${DEPS} " in
    *" srt "*)
      case " ${DEPS} " in
        *" openssl "*" mbedtls "*) ;;
        *" openssl "*) ;;
        *" mbedtls "*) ;;
        *) DEPS=" mbedtls${DEPS}"; echo "==> note: srt needs a TLS library; added mbedtls automatically" ;;
      esac ;;
  esac
  # Canonical build order: TLS backends first (srt/librtmp compile against
  # them), then codecs, then the protocol stacks that consume them.
  ORDERED=""
  for dep in openssl mbedtls x264 fdk-aac opus librtmp srt; do
    case " ${DEPS} " in
      *" ${dep} "*) ORDERED="${ORDERED} ${dep}" ;;
    esac
  done
  DEPS="${ORDERED}"
  echo "==> Optional dependencies: ${DEPS}"
fi

FF_EXTRA_FLAGS=""
fetch_git_dep() {  # <name>
  local dep="$1" tag pin dest head attempt
  tag="$(dep_tag "${dep}")"
  pin="$(dep_commit "${dep}")"
  dest="${SRC_DIR}/deps/${dep}"
  if [ -d "${dest}/.git" ]; then
    head="$(git -C "${dest}" rev-parse HEAD 2>/dev/null || true)"
    if [ "${head}" = "${pin}" ]; then
      echo "==> ${dep}: source already pinned at ${head:0:12}"
      return
    fi
    echo "==> ${dep}: source at ${head:-<broken>} does not match pin ${pin:0:12}; re-cloning"
    rm -rf "${dest}"
  fi
  echo "==> Fetching ${dep} (${tag:-HEAD}) from $(dep_git_url "${dep}")"
  # Cloning is the step most exposed to flaky networks and it takes minutes
  # to reach, so retry with backoff instead of failing a 20-minute chain.
  for attempt in 1 2 3 4 5 6; do
    rm -rf "${dest}"
    if [ -n "${tag}" ]; then
      git -c http.version=HTTP/1.1 clone --depth 1 --single-branch --branch "${tag}" "$(dep_git_url "${dep}")" "${dest}" && break
    else
      git -c http.version=HTTP/1.1 clone --depth 1 --single-branch "$(dep_git_url "${dep}")" "${dest}" && break
    fi
    if [ "${attempt}" = 6 ]; then
      echo "error: git clone of ${dep} failed 6 times" >&2
      exit 1
    fi
    echo "    clone attempt ${attempt} failed; retrying in $((attempt * 20))s" >&2
    sleep $((attempt * 20))
  done
  head="$(git -C "${dest}" rev-parse HEAD)"
  if [ "${head}" != "${pin}" ]; then
    echo "error: ${dep} checked out at ${head} but the script pins ${pin}." >&2
    echo "       ${dep} is pinned to a moving ref (branch or HEAD snapshot); update" >&2
    echo "       dep_commit() to the new hash after inspecting what changed." >&2
    exit 1
  fi
}

build_dep() {  # <name>
  local dep="$1" dest stamp
  dest="${SRC_DIR}/deps/${dep}"
  stamp="${dest}/.avbase-built"
  if [ -f "${stamp}" ] && [ "$(cat "${stamp}")" = "${PREFIX}|$(dep_commit "${dep}")" ]; then
    echo "==> ${dep}: up to date in ${PREFIX}"
    return
  fi
  echo "==> Building ${dep} ($(dep_tag "${dep}"))"
  fetch_git_dep "${dep}"
  # A git checkout ships no generated configure (release tarballs do). Run
  # autoreconf directly rather than the project's autogen.sh where one
  # exists: opus's autogen unconditionally downloads DNN model data
  # (~170 MB) that none of the features we enable use.
  if [ ! -f "${dest}/configure" ] && [ -f "${dest}/configure.ac" ]; then
    (cd "${dest}" && autoreconf -isf)
  elif [ ! -f "${dest}/configure" ] && [ -f "${dest}/autogen.sh" ]; then
    (cd "${dest}" && ./autogen.sh)
  fi
  case "${dep}" in
    openssl)
      (cd "${dest}" \
        && ./Configure --prefix="${PREFIX}" --openssldir="${PREFIX}/ssl" \
             shared no-tests \
        && make -j"${JOBS}" \
        && make install_sw install_ssldirs) ;;
    mbedtls)
      (cd "${dest}" && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
           -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
           -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
           -DMBEDTLS_FATAL_WARNINGS=OFF \
        && cmake --build build -j"${JOBS}" \
        && cmake --install build) ;;
    x264)
      (cd "${dest}" && ./configure --prefix="${PREFIX}" \
           --enable-static --enable-pic --disable-cli --disable-opencl \
        && make -j"${JOBS}" && make install) ;;
    fdk-aac)
      (cd "${dest}" && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
           -DCMAKE_INSTALL_PREFIX="${PREFIX}" -DBUILD_SHARED_LIBS=OFF \
        && cmake --build build -j"${JOBS}" \
        && cmake --install build) ;;
    opus)
      (cd "${dest}" && ./configure --prefix="${PREFIX}" \
           --disable-shared --enable-static --disable-doc --disable-extra-programs \
           --disable-dred --disable-osce --disable-deep-plc \
        && make -j"${JOBS}" && make install) ;;
    librtmp)
      # Only the static library is wanted: the default target also links the
      # rtmpdump CLI, which drags in more than FFmpeg needs.
      local rtmp_sys="posix"
      case "$(uname -s)" in Darwin) rtmp_sys="macos" ;; esac
      (cd "${dest}/librtmp" \
        && PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" \
           make -j"${JOBS}" SYS="${rtmp_sys}" CRYPTO=OPENSSL librtmp.a)
      install -d "${PREFIX}/lib" "${PREFIX}/include/librtmp" "${PREFIX}/lib/pkgconfig"
      install -m 644 "${dest}/librtmp/librtmp.a" "${PREFIX}/lib/"
      for h in rtmp.h log.h amf.h http.h; do
        install -m 644 "${dest}/librtmp/${h}" "${PREFIX}/include/librtmp/"
      done
      cat > "${PREFIX}/lib/pkgconfig/librtmp.pc" <<EOF
prefix=${PREFIX}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include/librtmp

Name: librtmp
Description: RTMP implementation
Version: 2.4
URL: https://rtmpdump.mplayerhq.hu/
Libs: -L\${libdir} -lrtmp -lssl -lcrypto -lz
Cflags: -I\${includedir}
EOF
      ;;
    srt)
      local enclib="mbedtls"
      case " ${DEPS} " in
        *" openssl "*) enclib="openssl" ;;
      esac
      (cd "${dest}" && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
           -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
           -DENABLE_APPS=OFF -DENABLE_TEST=OFF -DENABLE_TESTTOOLS=OFF \
           -DUSE_ENCLIB="${enclib}" \
        && cmake --build build -j"${JOBS}" \
        && cmake --install build) ;;
  esac
  echo "$(dep_commit "${dep}")|${PREFIX}" > "${stamp}"
}

# Encoder dependencies are only useful with a muxer to write through, so the
# moment one is selected the minimal encode-side muxer set comes along.
for dep in ${DEPS}; do
  build_dep "${dep}"
done

# FFmpeg flags contributed by the selected dependencies. TLS backend: at most
# one reaches FFmpeg (openssl wins; mbedtls may still have been built for srt).
case " ${DEPS} " in
  *" openssl "*) FF_EXTRA_FLAGS="${FF_EXTRA_FLAGS} --enable-openssl --enable-protocol=tls,https" ;;
  *" mbedtls "*) FF_EXTRA_FLAGS="${FF_EXTRA_FLAGS} --enable-mbedtls --enable-protocol=tls,https" ;;
esac
case " ${DEPS} " in
  *" x264 "*|*" fdk-aac "*) FF_EXTRA_FLAGS="${FF_EXTRA_FLAGS} --enable-muxer=mp4,matroska,adts --enable-encoder=aac" ;;
esac
case " ${DEPS} " in
  *" x264 "*)    FF_EXTRA_FLAGS="${FF_EXTRA_FLAGS} --enable-gpl --enable-libx264 --enable-encoder=libx264" ;;
esac
case " ${DEPS} " in
  *" fdk-aac "*) FF_EXTRA_FLAGS="${FF_EXTRA_FLAGS} --enable-nonfree --enable-libfdk-aac --enable-encoder=libfdk_aac" ;;
esac
case " ${DEPS} " in
  *" opus "*)    FF_EXTRA_FLAGS="${FF_EXTRA_FLAGS} --enable-libopus --enable-encoder=libopus --enable-decoder=libopus" ;;
esac
case " ${DEPS} " in
  *" librtmp "*) FF_EXTRA_FLAGS="${FF_EXTRA_FLAGS} --enable-librtmp --enable-protocol=librtmp,librtmpe,librtmps,librtmpt,librtmpte,librtmpts" ;;
esac
case " ${DEPS} " in
  *" srt "*)     FF_EXTRA_FLAGS="${FF_EXTRA_FLAGS} --enable-libsrt --enable-protocol=libsrt" ;;
esac

# ------------------------------------------------------------------- ffmpeg --
mkdir -p "${DOWNLOADS}"

echo "==> Fetching ${SRC_URL}"
if [ -f "${TARBALL}" ] && [ -n "${EXPECTED_SHA256}" ] \
   && echo "${EXPECTED_SHA256}  ${TARBALL}" | shasum -a 256 -c >/dev/null 2>&1; then
  echo "    reusing cached ${TARBALL} (sha256 ok)"
elif [ -f "${TARBALL}" ] && [ -z "${EXPECTED_SHA256}" ]; then
  echo "    reusing cached ${TARBALL} (no pin for ${VERSION}; set AVBASE_FFMPEG_SHA256)"
else
  if command -v curl >/dev/null 2>&1; then
    curl -fSL --retry 3 -o "${TARBALL}.part" "${SRC_URL}"
  else
    python3 - "${SRC_URL}" "${TARBALL}.part" <<'PY'
import sys, urllib.request
url, dst = sys.argv[1], sys.argv[2]
req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
with urllib.request.urlopen(req, timeout=300) as r, open(dst, "wb") as f:
    while chunk := r.read(1 << 20):
        f.write(chunk)
PY
  fi
  mv "${TARBALL}.part" "${TARBALL}"
fi

if [ -n "${EXPECTED_SHA256}" ]; then
  echo "==> Verifying sha256"
  echo "${EXPECTED_SHA256}  ${TARBALL}" | shasum -a 256 -c -
else
  ACTUAL="$(shasum -a 256 "${TARBALL}" | awk '{print $1}')"
  echo "==> WARNING: no pinned sha256 for ${VERSION}; got ${ACTUAL}"
  echo "             pin it in pinned_sha256() or export AVBASE_FFMPEG_SHA256=${ACTUAL}"
fi

SRC_TREE="${SRC_DIR}/ffmpeg-${VERSION}"
EXTRACT_STAMP="${SRC_TREE}/.avbase-extracted"
if [ -f "${EXTRACT_STAMP}" ]; then
  echo "==> Reusing extracted source at ${SRC_TREE}"
else
  echo "==> Extracting into ${SRC_TREE}"
  rm -rf "${SRC_TREE}"
  mkdir -p "${SRC_TREE}"
  tar -xf "${TARBALL}" -C "${SRC_TREE}" --strip-components=1
  touch "${EXTRACT_STAMP}"
fi

# NOTE: without AVBASE_FFMPEG_DEPS there is no TLS backend, so https/tls are
# unavailable (tests use file:// and http:// only). Add "openssl" (or
# "mbedtls") to AVBASE_FFMPEG_DEPS to switch them on.
configure_flags() {
  cat <<FLAGS
--prefix="${PREFIX}"
--disable-programs
--disable-doc
--disable-debug
--disable-autodetect
--disable-x86asm
--disable-avfilter
--disable-avdevice
--disable-postproc
--disable-everything
--enable-network
--enable-protocol=file,http,tcp,httpproxy,crypto
--enable-demuxer=mov,matroska,mpegts,flv,mp3,wav,ogg,aac,h264,hevc,avi,mpegps
--enable-parser=h264,hevc,aac,mp3,vp9,opus,mpeg4video
--enable-decoder=h264,hevc,aac,mp3,vp9,opus,mpeg4
--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata
--enable-muxer=null
--enable-filter=null
--enable-swresample
--enable-swscale
FLAGS
}

# FFmpeg cannot build static and shared in one configure pass, so "both" is
# two out-of-tree passes into the same prefix (.a and .dylib/.so coexist).
build_one() {
  local mode="$1"
  local extra_flags=()
  case "${mode}" in
    shared) extra_flags=(--enable-shared --disable-static) ;;
    static) extra_flags=(--enable-static --disable-shared --enable-pic) ;;
  esac

  local build_dir="${SRC_DIR}/ffmpeg-${VERSION}-build-${mode}"
  local stamp="${build_dir}/.avbase-configured"

  echo "==> Configuring (${mode}) into ${build_dir}"
  if [ -f "${stamp}" ] && [ "$(cat "${stamp}")" = "${PREFIX}|${VERSION}|${mode}|${DEPS}|${FF_EXTRA_FLAGS}" ]; then
    echo "    configure stamp matches, skipping (delete ${build_dir} to force)"
  else
    rm -rf "${build_dir}"
    mkdir -p "${build_dir}"
    (cd "${build_dir}" && PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" \
        "${SRC_TREE}/configure" $(configure_flags) "${extra_flags[@]}" ${FF_EXTRA_FLAGS})
    echo "${PREFIX}|${VERSION}|${mode}|${DEPS}|${FF_EXTRA_FLAGS}" > "${stamp}"
  fi

  echo "==> Building (${mode}) with -j${JOBS}"
  make -C "${build_dir}" -j"${JOBS}"
  make -C "${build_dir}" install
}

if [ "${LINK}" = "shared" ] || [ "${LINK}" = "both" ]; then
  build_one shared
fi
if [ "${LINK}" = "static" ] || [ "${LINK}" = "both" ]; then
  build_one static
fi

echo "==> Installed:"
ls "${PREFIX}/lib" | grep -E '\.(so\.[0-9]+|dylib|a)$' | head -40 || true
grep -m1 LIBAVCODEC_VERSION_MAJOR "${PREFIX}/include/libavcodec/version_major.h"
echo
echo "Now run:  cmake --preset linux-ffmpeg711   (or -DAVBASE_FFMPEG_ROOT=${PREFIX})"
