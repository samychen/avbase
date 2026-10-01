# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

option(AVBASE_ENABLE_FFMPEG       "Build the FFmpeg demuxer/decoder adapters" OFF)
option(AVBASE_ENABLE_SDL2         "Build the SDL2 video/audio backend"        OFF)
option(AVBASE_ENABLE_LINUX_NATIVE "Build the native Linux GL/X11/Wayland backend" OFF)
option(AVBASE_ENABLE_ANDROID      "Build the Android backend"                 OFF)
option(AVBASE_ENABLE_IOS          "Build the iOS backend"                     OFF)
option(AVBASE_ENABLE_CAPI         "Build the C ABI compatibility layer"       OFF)
option(AVBASE_BUILD_SHARED        "Build shared libraries instead of static"  OFF)

option(AVBASE_BUILD_TESTS    "Build unit/contract/integration tests" ${AVBASE_IS_TOP_LEVEL})
option(AVBASE_BUILD_EXAMPLES "Build example programs"               ${AVBASE_IS_TOP_LEVEL})
option(AVBASE_BUILD_BENCH    "Build benchmarks"                     OFF)
option(AVBASE_BUILD_FUZZ     "Build libFuzzer targets"              OFF)
option(AVBASE_INSTALL        "Generate install rules"               ${AVBASE_IS_TOP_LEVEL})

option(AVBASE_STRICT_WARNINGS "Enable -Wconversion / -Wold-style-cast" OFF)
option(AVBASE_WERROR          "Treat warnings as errors"               OFF)
option(AVBASE_ENABLE_LTO      "Enable IPO/LTO for release builds"      OFF)
option(AVBASE_COVERAGE        "Enable gcov instrumentation"            OFF)
option(AVBASE_ENABLE_DCHECK   "Enable DCHECK / SEQUENCE_CHECKER"       ON)

set(AVBASE_SANITIZERS "" CACHE STRING "Semicolon list: address;thread;undefined")
set(AVBASE_FFMPEG_ROOT "" CACHE PATH "Prefix where FFmpeg is installed")

if("address" IN_LIST AVBASE_SANITIZERS AND "thread" IN_LIST AVBASE_SANITIZERS)
  message(FATAL_ERROR "ASan and TSan cannot be enabled together")
endif()
if(AVBASE_ENABLE_ANDROID AND AVBASE_ENABLE_IOS)
  message(FATAL_ERROR "Android and iOS backends are mutually exclusive")
endif()

# NOTE: AVBASE_BUILD_TESTS is deliberately *not* gated on AVBASE_ENABLE_FFMPEG.
# Building and testing base/ + media/ + player/ without FFmpeg installed is the
# executable proof of design goal G2; see docs/06 §2.
