# Copyright 2026 The ijkpp Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.20)

# VERSION.txt, not VERSION: the repo root is also the include root (-I.),
# and on Apple's case-insensitive filesystem a file literally named VERSION
# shadows libc++'s <version> header inside every standard include.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" IJKPP_VERSION_RAW)
string(STRIP "${IJKPP_VERSION_RAW}" IJKPP_VERSION)

project(ijkpp
    VERSION   ${IJKPP_VERSION}
    LANGUAGES C CXX
    DESCRIPTION "A C++20 reimplementation of the ijkplayer core, Chromium-style")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(IJKPP_IS_TOP_LEVEL ON)
else()
  set(IJKPP_IS_TOP_LEVEL OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(IjkppOptions)
include(IjkppCompilerFlags)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # Google style: no GNU extensions.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ${IJKPP_IS_TOP_LEVEL})

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

find_package(Threads REQUIRED)

if(IJKPP_ENABLE_FFMPEG)
  find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil swscale swresample)
endif()
if(IJKPP_BUILD_TESTS)
  include(IjkppThirdParty)
endif()

configure_file(cmake/BuildConfig.h.in "${CMAKE_BINARY_DIR}/generated/ijkpp/BuildConfig.h" @ONLY)
configure_file(cmake/Version.h.in     "${CMAKE_BINARY_DIR}/generated/ijkpp/Version.h"     @ONLY)

add_subdirectory(base)
add_subdirectory(media)
add_subdirectory(player)
add_subdirectory(platform)
if(IJKPP_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
if(IJKPP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  # Developer CLI. Only meaningful with FFmpeg: it drives the real demuxer and
  # decoders, and a version built on fakes would prove nothing.
  if(IJKPP_ENABLE_FFMPEG)
    add_subdirectory(tools/inspect)
  endif()
endif()

include(IjkppCheckInvariants)

if(IJKPP_IS_TOP_LEVEL)
  message(STATUS "--------------------------------------------------------")
  message(STATUS " ijkpp ${PROJECT_VERSION}   (${CMAKE_BUILD_TYPE})")
  message(STATUS "   compiler         : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
  message(STATUS "   exceptions / rtti: OFF / OFF   (Google style)")
  message(STATUS "   DCHECK           : ${IJKPP_ENABLE_DCHECK}")
  message(STATUS "   FFmpeg           : ${IJKPP_ENABLE_FFMPEG}  ${FFmpeg_VERSION_STRING}")
  message(STATUS "   sanitizers       : ${IJKPP_SANITIZERS}")
  message(STATUS "   tests            : ${IJKPP_BUILD_TESTS}")
  message(STATUS "--------------------------------------------------------")
endif()
