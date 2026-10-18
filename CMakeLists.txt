# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.20)

# VERSION.txt, not VERSION: the repo root is also the include root (-I.),
# and on Apple's case-insensitive filesystem a file literally named VERSION
# shadows libc++'s <version> header inside every standard include.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" AVBASE_VERSION_RAW)
string(STRIP "${AVBASE_VERSION_RAW}" AVBASE_VERSION)

project(avbase
    VERSION   ${AVBASE_VERSION}
    LANGUAGES C CXX
    DESCRIPTION "A C++20 reimplementation of the ijkplayer core, Chromium-style")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(AVBASE_IS_TOP_LEVEL ON)
else()
  set(AVBASE_IS_TOP_LEVEL OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(AvbaseOptions)
include(AvbaseCompilerFlags)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # Google style: no GNU extensions.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ${AVBASE_IS_TOP_LEVEL})

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

find_package(Threads REQUIRED)

if(AVBASE_ENABLE_FFMPEG)
  find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil swscale swresample avfilter)
endif()
if(AVBASE_BUILD_TESTS)
  include(AvbaseThirdParty)
endif()

configure_file(cmake/BuildConfig.h.in "${CMAKE_BINARY_DIR}/generated/avbase/BuildConfig.h" @ONLY)
configure_file(cmake/Version.h.in     "${CMAKE_BINARY_DIR}/generated/avbase/Version.h"     @ONLY)

add_subdirectory(base)
# render/ publishes the standalone GL module (avbase_gl). It depends on nothing
# else in the tree, and media/gl links it, so it must come before media.
add_subdirectory(render)
add_subdirectory(media)
add_subdirectory(player)
# The C ABI (docs/08 M15) is its own optional target: a C/JNI host links it
# without acquiring the C++ headers. Off by default -- see docs/06 for the
# wiring contract. Its smoke test is registered further down, after
# enable_testing(); the directory itself is added there.
if(AVBASE_ENABLE_CAPI AND NOT AVBASE_BUILD_TESTS)
  add_subdirectory(player/public/c)
endif()
# platform/ supplies the window systems that plug into both of the above, so it
# comes last.
add_subdirectory(platform)
if(AVBASE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
# Host applications (apps/) are SDK CONSUMERS and are gated separately from the
# examples: an example proves the SDK is usable in thirty lines, an app is a
# product shell with its own toolkit dependency. Nothing in the core may include
# an apps/ header -- the same one-way rule as C28 for render/.
if(AVBASE_BUILD_QT_PLAYER)
  add_subdirectory(apps)
endif()
if(AVBASE_BUILD_TESTS)
  enable_testing()
  # After enable_testing() on purpose. The C ABI directory is added far above,
  # and add_test() in a subdirectory processed BEFORE testing was enabled is
  # silently dropped: the configure succeeds, ctest reports "No tests were
  # found", and the gate that looks present is not running anywhere. Adding it
  # here is the only spot that is both after enable_testing() and next to the
  # other test registration.
  if(AVBASE_ENABLE_CAPI)
    add_subdirectory(player/public/c)
  endif()
endif()
# The fuzz targets live beside the tests and reuse their support; building
# them alone (AVBASE_BUILD_FUZZ with AVBASE_BUILD_TESTS=OFF) is a valid
# configuration, so the directory follows either option.
if(AVBASE_BUILD_TESTS OR AVBASE_BUILD_FUZZ)
  add_subdirectory(tests)
endif()
if(AVBASE_BUILD_TESTS AND AVBASE_ENABLE_FFMPEG)
  # Developer CLI. Only meaningful with FFmpeg: it drives the real demuxer
  # and decoders, and a version built on fakes would prove nothing. Kept
  # under the tests gate (its asan/coverage runs are test infrastructure).
  add_subdirectory(tools/inspect)
endif()
if(AVBASE_BUILD_BENCH)
  # Self-contained micro-benchmarks (docs/04 §8, M13). No Google Benchmark,
  # no download; only avbase::base. Gated so the default build is unaffected.
  add_subdirectory(bench)
endif()

include(AvbaseCheckInvariants)
# Install/export/packaging rules (docs/06 §9, M13). Last on purpose: every
# target and option must exist by the time this reads them.
include(AvbaseInstall)

if(AVBASE_IS_TOP_LEVEL)
  message(STATUS "--------------------------------------------------------")
  message(STATUS " avbase ${PROJECT_VERSION}   (${CMAKE_BUILD_TYPE})")
  message(STATUS "   compiler         : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
  message(STATUS "   exceptions / rtti: OFF / OFF   (Google style)")
  message(STATUS "   DCHECK           : ${AVBASE_ENABLE_DCHECK}")
  message(STATUS "   FFmpeg           : ${AVBASE_ENABLE_FFMPEG}  ${FFmpeg_VERSION_STRING}")
  message(STATUS "   sanitizers       : ${AVBASE_SANITIZERS}")
  message(STATUS "   tests            : ${AVBASE_BUILD_TESTS}")
  message(STATUS "--------------------------------------------------------")
endif()
