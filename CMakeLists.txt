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
  find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil swscale swresample)
endif()
if(AVBASE_BUILD_TESTS)
  include(AvbaseThirdParty)
endif()

configure_file(cmake/BuildConfig.h.in "${CMAKE_BINARY_DIR}/generated/avbase/BuildConfig.h" @ONLY)
configure_file(cmake/Version.h.in     "${CMAKE_BINARY_DIR}/generated/avbase/Version.h"     @ONLY)

add_subdirectory(base)
add_subdirectory(media)
add_subdirectory(player)
add_subdirectory(platform)
if(AVBASE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(AVBASE_BUILD_TESTS)
  enable_testing()
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

include(AvbaseCheckInvariants)

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
