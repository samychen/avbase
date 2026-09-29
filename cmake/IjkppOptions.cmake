# Copyright 2026 The ijkpp Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

option(IJKPP_ENABLE_FFMPEG       "Build the FFmpeg demuxer/decoder adapters" OFF)
option(IJKPP_ENABLE_SDL2         "Build the SDL2 video/audio backend"        OFF)
option(IJKPP_ENABLE_LINUX_NATIVE "Build the native Linux GL/X11/Wayland backend" OFF)
option(IJKPP_ENABLE_ANDROID      "Build the Android backend"                 OFF)
option(IJKPP_ENABLE_IOS          "Build the iOS backend"                     OFF)
option(IJKPP_ENABLE_CAPI         "Build the C ABI compatibility layer"       OFF)
option(IJKPP_BUILD_SHARED        "Build shared libraries instead of static"  OFF)

option(IJKPP_BUILD_TESTS    "Build unit/contract/integration tests" ${IJKPP_IS_TOP_LEVEL})
option(IJKPP_BUILD_EXAMPLES "Build example programs"               ${IJKPP_IS_TOP_LEVEL})
option(IJKPP_BUILD_BENCH    "Build benchmarks"                     OFF)
option(IJKPP_BUILD_FUZZ     "Build libFuzzer targets"              OFF)
option(IJKPP_INSTALL        "Generate install rules"               ${IJKPP_IS_TOP_LEVEL})

option(IJKPP_STRICT_WARNINGS "Enable -Wconversion / -Wold-style-cast" OFF)
option(IJKPP_WERROR          "Treat warnings as errors"               OFF)
option(IJKPP_ENABLE_LTO      "Enable IPO/LTO for release builds"      OFF)
option(IJKPP_COVERAGE        "Enable gcov instrumentation"            OFF)
option(IJKPP_ENABLE_DCHECK   "Enable DCHECK / SEQUENCE_CHECKER"       ON)

set(IJKPP_SANITIZERS "" CACHE STRING "Semicolon list: address;thread;undefined")
set(IJKPP_FFMPEG_ROOT "" CACHE PATH "Prefix where FFmpeg is installed")

if("address" IN_LIST IJKPP_SANITIZERS AND "thread" IN_LIST IJKPP_SANITIZERS)
  message(FATAL_ERROR "ASan and TSan cannot be enabled together")
endif()
if(IJKPP_ENABLE_ANDROID AND IJKPP_ENABLE_IOS)
  message(FATAL_ERROR "Android and iOS backends are mutually exclusive")
endif()

# NOTE: IJKPP_BUILD_TESTS is deliberately *not* gated on IJKPP_ENABLE_FFMPEG.
# Building and testing base/ + media/ + player/ without FFmpeg installed is the
# executable proof of design goal G2; see docs/06 §2.
