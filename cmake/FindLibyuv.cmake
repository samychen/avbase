# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# FindLibyuv
# ----------
# Imported target: libyuv::libyuv
#
# libyuv is an OPTIONAL dependency: it accelerates pixel-format conversion
# (media/ffmpeg/video_convert.cc) and is built by tools/setup_ffmpeg.sh
# into the SAME prefix as FFmpeg. So the hint chain mirrors FindFFmpeg's:
#   1. AVBASE_FFMPEG_ROOT (cache var or env) — the deps prefix by definition
#   2. the repo-local tools/build prefix (same convention as FindFFmpeg)
#   3. pkg-config (system installs ship libyuv.pc)
# When nothing is found the caller falls back to sws_scale, so NOT_FOUND is
# not an error and this module never fails.

set(_avbase_libyuv_hints "")
if(AVBASE_FFMPEG_ROOT)
  list(APPEND _avbase_libyuv_hints "${AVBASE_FFMPEG_ROOT}")
elseif(DEFINED ENV{AVBASE_FFMPEG_ROOT})
  list(APPEND _avbase_libyuv_hints "$ENV{AVBASE_FFMPEG_ROOT}")
elseif(DEFINED ENV{FFMPEG_ROOT})
  list(APPEND _avbase_libyuv_hints "$ENV{FFMPEG_ROOT}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../tools/build/include/libyuv/version.h")
  # CMAKE_CURRENT_LIST_DIR, not CMAKE_CURRENT_SOURCE_DIR: this module is
  # included from platform/CMakeLists, and the tools/build convention is
  # relative to the repo root (this file's own location).
  list(APPEND _avbase_libyuv_hints "${CMAKE_CURRENT_LIST_DIR}/../tools/build")
endif()

find_path(Libyuv_INCLUDE_DIR NAMES libyuv/version.h
    HINTS ${_avbase_libyuv_hints} PATH_SUFFIXES include)
find_library(Libyuv_LIBRARY NAMES yuv libyuv
    HINTS ${_avbase_libyuv_hints} PATH_SUFFIXES lib lib64)

if(Libyuv_INCLUDE_DIR AND Libyuv_LIBRARY)
  set(Libyuv_FOUND TRUE)
  file(STRINGS "${Libyuv_INCLUDE_DIR}/libyuv/version.h" _libyuv_version_line
       REGEX "#define LIBYUV_VERSION [0-9]+")
  string(REGEX REPLACE ".*LIBYUV_VERSION[ \t]+([0-9]+).*" "\\1"
         Libyuv_VERSION "${_libyuv_version_line}")
  add_library(libyuv::libyuv UNKNOWN IMPORTED)
  set_target_properties(libyuv::libyuv PROPERTIES
      IMPORTED_LOCATION "${Libyuv_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Libyuv_INCLUDE_DIR}")
  mark_as_advanced(Libyuv_INCLUDE_DIR Libyuv_LIBRARY)
  message(STATUS "FindLibyuv: libyuv ${Libyuv_VERSION} at ${Libyuv_INCLUDE_DIR}")
else()
  set(Libyuv_FOUND FALSE)
  message(STATUS "FindLibyuv: libyuv not found — video conversion uses "
                 "sws_scale only (add 'libyuv' to AVBASE_FFMPEG_DEPS to enable)")
endif()
