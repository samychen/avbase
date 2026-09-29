# Copyright 2026 The ijkpp Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# FindFFmpeg
# ----------
# Imported targets: FFmpeg::avformat / avcodec / avutil / swscale / swresample
# Result variables: FFmpeg_FOUND, FFmpeg_<comp>_FOUND, FFmpeg_<comp>_VERSION_MAJOR,
#                   FFmpeg_INCLUDE_DIRS, FFmpeg_LIBRARIES, IJKPP_FFMPEG_MAJOR
#
# Three strategies, in order:
#   1. CMake config package (vcpkg, some distros)
#   2. pkg-config           (distro packages; skipped when IJKPP_FFMPEG_ROOT is
#                            set, so a pinned build always wins over whatever
#                            the system happens to have)
#   3. Manual find_path/find_library (NDK, xcframework, tools/setup_ffmpeg.sh)

if(NOT FFmpeg_FIND_COMPONENTS)
  set(FFmpeg_FIND_COMPONENTS avformat avcodec avutil swscale swresample)
endif()

set(_ijkpp_ff_hints "")
if(IJKPP_FFMPEG_ROOT)
  list(APPEND _ijkpp_ff_hints "${IJKPP_FFMPEG_ROOT}")
elseif(DEFINED ENV{FFMPEG_ROOT})
  list(APPEND _ijkpp_ff_hints "$ENV{FFMPEG_ROOT}")
endif()

# ---- 1. CMake config package ------------------------------------------------
find_package(FFMPEG CONFIG QUIET HINTS ${_ijkpp_ff_hints})
if(FFMPEG_FOUND)
  foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
    if(TARGET FFMPEG::${_comp} AND NOT TARGET FFmpeg::${_comp})
      add_library(FFmpeg::${_comp} INTERFACE IMPORTED)
      target_link_libraries(FFmpeg::${_comp} INTERFACE FFMPEG::${_comp})
      set(FFmpeg_${_comp}_FOUND TRUE)
    endif()
  endforeach()
endif()

# ---- 2. pkg-config ----------------------------------------------------------
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND AND NOT _ijkpp_ff_hints AND NOT ANDROID AND NOT IOS)
  foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
    if(TARGET FFmpeg::${_comp})
      continue()
    endif()
    pkg_check_modules(PC_${_comp} QUIET IMPORTED_TARGET lib${_comp})
    if(PC_${_comp}_FOUND)
      add_library(FFmpeg::${_comp} INTERFACE IMPORTED)
      target_link_libraries(FFmpeg::${_comp} INTERFACE PkgConfig::PC_${_comp})
      string(REGEX REPLACE "^([0-9]+).*" "\\1" FFmpeg_${_comp}_VERSION_MAJOR
             "${PC_${_comp}_VERSION}")
      set(FFmpeg_${_comp}_FOUND TRUE)
      set(FFmpeg_${_comp}_VERSION "${PC_${_comp}_VERSION}")
    endif()
  endforeach()
endif()

# ---- 3. Manual --------------------------------------------------------------
foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
  if(TARGET FFmpeg::${_comp})
    continue()
  endif()
  find_path(FFmpeg_${_comp}_INCLUDE_DIR
      NAMES "lib${_comp}/${_comp}.h"
      HINTS ${_ijkpp_ff_hints}
      PATH_SUFFIXES include)
  find_library(FFmpeg_${_comp}_LIBRARY
      NAMES ${_comp} lib${_comp}
      HINTS ${_ijkpp_ff_hints}
      PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu)
  if(FFmpeg_${_comp}_INCLUDE_DIR AND FFmpeg_${_comp}_LIBRARY)
    set(FFmpeg_${_comp}_FOUND TRUE)
    # Parse the version from the headers rather than trusting pkg-config, which
    # is absent for cross-compiled and setup_ffmpeg.sh builds.
    # The macro name is upper-cased (LIBAVCODEC_VERSION_MAJOR), so the
    # component name must be upper-cased before it is interpolated into the
    # regex — otherwise the version silently parses as empty and the minimum
    # version gate below cannot fire.
    string(TOUPPER "${_comp}" _comp_upper)
    set(_vfile "${FFmpeg_${_comp}_INCLUDE_DIR}/lib${_comp}/version_major.h")
    if(NOT EXISTS "${_vfile}")
      set(_vfile "${FFmpeg_${_comp}_INCLUDE_DIR}/lib${_comp}/version.h")
    endif()
    file(STRINGS "${_vfile}" _vline
         REGEX "^#define[ \t]+LIB${_comp_upper}_VERSION_MAJOR[ \t]+[0-9]+")
    string(REGEX REPLACE ".*_MAJOR[ \t]+([0-9]+).*" "\\1"
           FFmpeg_${_comp}_VERSION_MAJOR "${_vline}")
    set(FFmpeg_${_comp}_VERSION "${FFmpeg_${_comp}_VERSION_MAJOR}.x")
    add_library(FFmpeg::${_comp} UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::${_comp} PROPERTIES
        IMPORTED_LOCATION "${FFmpeg_${_comp}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_${_comp}_INCLUDE_DIR}")
    mark_as_advanced(FFmpeg_${_comp}_INCLUDE_DIR FFmpeg_${_comp}_LIBRARY)
  endif()
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
    REQUIRED_VARS FFmpeg_avutil_FOUND
    HANDLE_COMPONENTS
    VERSION_VAR FFmpeg_avformat_VERSION)

if(FFmpeg_FOUND)
  set(FFmpeg_LIBRARIES "")
  set(FFmpeg_INCLUDE_DIRS "")
  foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
    if(TARGET FFmpeg::${_comp})
      list(APPEND FFmpeg_LIBRARIES FFmpeg::${_comp})
      get_target_property(_inc FFmpeg::${_comp} INTERFACE_INCLUDE_DIRECTORIES)
      if(_inc)
        list(APPEND FFmpeg_INCLUDE_DIRS ${_inc})
      endif()
    endif()
  endforeach()
  list(REMOVE_DUPLICATES FFmpeg_INCLUDE_DIRS)
  set(FFmpeg_LIBRARIES   "${FFmpeg_LIBRARIES}"   CACHE INTERNAL "")
  set(FFmpeg_INCLUDE_DIRS "${FFmpeg_INCLUDE_DIRS}" CACHE INTERNAL "")
  set(IJKPP_FFMPEG_MAJOR "${FFmpeg_avcodec_VERSION_MAJOR}" CACHE INTERNAL "")
  set(FFmpeg_VERSION_STRING "${FFmpeg_avformat_VERSION}" CACHE INTERNAL "")

  # Minimum supported version. Below this the compat layer would need branches
  # for APIs that predate AVPacket's int64 timestamps; not worth carrying.
  if(IJKPP_FFMPEG_MAJOR LESS 58)
    message(FATAL_ERROR
        "ijkpp requires FFmpeg >= 4.4 (libavcodec >= 58); found libavcodec "
        "${IJKPP_FFMPEG_MAJOR}. Point IJKPP_FFMPEG_ROOT at a newer build, or "
        "run tools/setup_ffmpeg.sh. See docs/06 §5 for the support matrix.")
  endif()
  message(STATUS "FindFFmpeg: libavcodec major ${IJKPP_FFMPEG_MAJOR}"
                 " (avformat ${FFmpeg_avformat_VERSION}) at ${FFmpeg_INCLUDE_DIRS}")
endif()
