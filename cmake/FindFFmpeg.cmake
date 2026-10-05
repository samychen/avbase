# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# FindFFmpeg
# ----------
# Imported targets: FFmpeg::avformat / avcodec / avutil / swscale / swresample
# Result variables: FFmpeg_FOUND, FFmpeg_<comp>_FOUND, FFmpeg_<comp>_VERSION_MAJOR,
#                   FFmpeg_INCLUDE_DIRS, FFmpeg_LIBRARIES, AVBASE_FFMPEG_MAJOR
#
# Three strategies, in order:
#   1. CMake config package (vcpkg, some distros)
#   2. pkg-config           (distro packages; skipped when AVBASE_FFMPEG_ROOT is
#                            set, so a pinned build always wins over whatever
#                            the system happens to have)
#   3. Manual find_path/find_library (NDK, xcframework, tools/setup_ffmpeg.sh)

if(NOT FFmpeg_FIND_COMPONENTS)
  set(FFmpeg_FIND_COMPONENTS avformat avcodec avutil swscale swresample)
endif()

set(_avbase_ff_hints "")
if(AVBASE_FFMPEG_ROOT)
  list(APPEND _avbase_ff_hints "${AVBASE_FFMPEG_ROOT}")
elseif(DEFINED ENV{FFMPEG_ROOT})
  list(APPEND _avbase_ff_hints "$ENV{FFMPEG_ROOT}")
endif()

# ---- 1. CMake config package ------------------------------------------------
find_package(FFMPEG CONFIG QUIET HINTS ${_avbase_ff_hints})
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
if(PKG_CONFIG_FOUND AND NOT _avbase_ff_hints AND NOT ANDROID AND NOT IOS)
  foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
    if(TARGET FFmpeg::${_comp})
      continue()
    endif()
    pkg_check_modules(PC_${_comp} QUIET IMPORTED_TARGET lib${_comp})
    message(STATUS "[FindFFmpeg debug] ${_comp}: found=${PC_${_comp}_FOUND} "
        "inc=${PC_${_comp}_INCLUDE_DIRS} libs=${PC_${_comp}_LINK_LIBRARIES} "
        "libdirs=${PC_${_comp}_LIBRARY_DIRS}")
    if(PC_${_comp}_FOUND)
      # Built directly from the PC_* variables rather than linking the
      # PkgConfig:: target: on Homebrew/Apple Silicon the PkgConfig target's
      # include usage requirements did not survive the INTERFACE hop, which
      # left av_includes.h unfindable with a successful-looking find. The
      # variables are authoritative either way.
      add_library(FFmpeg::${_comp} INTERFACE IMPORTED)
      set_property(TARGET FFmpeg::${_comp} PROPERTY
          INTERFACE_INCLUDE_DIRECTORIES "${PC_${_comp}_INCLUDE_DIRS}")
      set_property(TARGET FFmpeg::${_comp} PROPERTY
          INTERFACE_LINK_DIRECTORIES "${PC_${_comp}_LIBRARY_DIRS}")
      set_property(TARGET FFmpeg::${_comp} PROPERTY
          INTERFACE_LINK_LIBRARIES "${PC_${_comp}_LINK_LIBRARIES}")
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
      HINTS ${_avbase_ff_hints}
      PATH_SUFFIXES include)
  find_library(FFmpeg_${_comp}_LIBRARY
      NAMES ${_comp} lib${_comp}
      HINTS ${_avbase_ff_hints}
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
  set(AVBASE_FFMPEG_MAJOR "${FFmpeg_avcodec_VERSION_MAJOR}" CACHE INTERNAL "")
  set(FFmpeg_VERSION_STRING "${FFmpeg_avformat_VERSION}" CACHE INTERNAL "")

  # Minimum supported version. Below this the compat layer would need branches
  # for APIs that predate AVPacket's int64 timestamps; not worth carrying.
  if(AVBASE_FFMPEG_MAJOR LESS 58)
    message(FATAL_ERROR
        "avbase requires FFmpeg >= 4.4 (libavcodec >= 58); found libavcodec "
        "${AVBASE_FFMPEG_MAJOR}. Point AVBASE_FFMPEG_ROOT at a newer build, or "
        "run tools/setup_ffmpeg.sh. See docs/06 §5 for the support matrix.")
  endif()
  message(STATUS "FindFFmpeg: libavcodec major ${AVBASE_FFMPEG_MAJOR}"
                 " (avformat ${FFmpeg_avformat_VERSION}) at ${FFmpeg_INCLUDE_DIRS}")

  # ---- 4. Header/library consistency probe --------------------------------
  # Everything above reads the HEADERS; nothing yet guarantees that the
  # library actually linked is from the same build. FFmpeg tolerates the
  # mismatch at compile time and dies at runtime (struct layout drift ->
  # SIGBUS; the classic case is a pinned include dir with a distro dylib
  # picked up by the linker). Compile and run a trivial probe that reports
  # the library's own avcodec_version() and compare it with the headers.
  option(AVBASE_SKIP_FFMPEG_PROBE
      "Skip the configure-time FFmpeg header/library consistency probe" OFF)
  if(CMAKE_CROSSCOMPILING)
    message(STATUS "FindFFmpeg: header/library probe skipped (cross-compiling)")
  elseif(AVBASE_SKIP_FFMPEG_PROBE)
    message(STATUS "FindFFmpeg: header/library probe skipped (AVBASE_SKIP_FFMPEG_PROBE)")
  else()
    set(_probe_src "${CMAKE_BINARY_DIR}/CMakeFiles/avbase-ffmpeg-version-probe.c")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/FFmpegVersionProbe.c"
                   "${_probe_src}" COPYONLY)

    # Header major, parsed here because strategies 1/2 don't always set
    # FFmpeg_avcodec_VERSION_MAJOR from the headers.
    set(_probe_header "")
    foreach(_inc IN LISTS FFmpeg_INCLUDE_DIRS)
      if(EXISTS "${_inc}/libavcodec/version_major.h")
        set(_probe_header "${_inc}/libavcodec/version_major.h")
        break()
      endif()
      if(EXISTS "${_inc}/libavcodec/version.h")
        set(_probe_header "${_inc}/libavcodec/version.h")
        break()
      endif()
    endforeach()
    set(_probe_linkitems "")
    set(_probe_linkdirs "")
    if(_probe_header)
      file(STRINGS "${_probe_header}" _probe_vline REGEX
           "^#define[ \t]+LIBAVCODEC_VERSION_MAJOR[ \t]+[0-9]+")
      string(REGEX REPLACE ".*_MAJOR[ \t]+([0-9]+).*" "\\1"
             _probe_header_major "${_probe_vline}")
    endif()
    foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
      if(FFmpeg_${_comp}_LIBRARY AND EXISTS "${FFmpeg_${_comp}_LIBRARY}")
        list(APPEND _probe_linkitems "${FFmpeg_${_comp}_LIBRARY}")
        get_filename_component(_libdir "${FFmpeg_${_comp}_LIBRARY}" DIRECTORY)
        list(APPEND _probe_linkdirs "${_libdir}")
      else()
        # No concrete file (pkg-config / config package): fall back to -l
        # flags plus whatever link directories the strategies recorded.
        get_target_property(_dirs FFmpeg::${_comp} INTERFACE_LINK_DIRECTORIES)
        if(_dirs)
          list(APPEND _probe_linkdirs ${_dirs})
        endif()
        if(PC_${_comp}_LINK_LIBRARIES)
          list(APPEND _probe_linkitems ${PC_${_comp}_LINK_LIBRARIES})
        else()
          list(APPEND _probe_linkitems "-l${_comp}")
        endif()
      endif()
    endforeach()

    if(NOT _probe_header)
      message(STATUS "FindFFmpeg: header/library probe skipped (no libavcodec headers found)")
    elseif(NOT _probe_linkitems)
      message(STATUS "FindFFmpeg: header/library probe skipped (no linkable avcodec library)")
    else()
      list(REMOVE_DUPLICATES _probe_linkdirs)
      set(_probe_dir_flags "")
      foreach(_dir IN LISTS _probe_linkdirs)
        list(APPEND _probe_dir_flags "-DLINK_DIRECTORIES:PATH=${_dir}")
      endforeach()
      try_run(_probe_run _probe_compiled
          "${CMAKE_BINARY_DIR}/CMakeTmp/avbase-ffmpeg-probe"
          "${_probe_src}"
          CMAKE_FLAGS
            "-DINCLUDE_DIRECTORIES:PATH=${FFmpeg_INCLUDE_DIRS}"
            "-DCMAKE_BUILD_RPATH:PATH=${_probe_linkdirs}"
            ${_probe_dir_flags}
          LINK_LIBRARIES ${_probe_linkitems}
          RUN_OUTPUT_VARIABLE _probe_output)
      if(NOT _probe_compiled)
        message(FATAL_ERROR
            "FindFFmpeg: the header/library consistency probe failed to COMPILE.\n"
            "Headers: ${_probe_header}\nLibraries: ${_probe_linkitems}\n"
            "This means the include/lib combination cannot be linked at all; "
            "check AVBASE_FFMPEG_ROOT / pkg-config paths. Re-run with "
            "-DAVBASE_SKIP_FFMPEG_PROBE=ON to bypass (at your own risk).")
      elseif(NOT _probe_run MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "FindFFmpeg: the header/library consistency probe FAILED TO RUN.\n"
            "Headers say libavcodec major ${_probe_header_major} but the binary "
            "crashed before reporting the library version — the classic "
            "headers-from-one-build / library-from-another mismatch that "
            "otherwise surfaces as SIGBUS at playback time. Point "
            "AVBASE_FFMPEG_ROOT at a consistent build. Re-run with "
            "-DAVBASE_SKIP_FFMPEG_PROBE=ON to bypass (at your own risk).")
      else()
        string(STRIP "${_probe_output}" _probe_output)
        math(EXPR _probe_lib_major "${_probe_output} / 65536")
        if(NOT _probe_lib_major EQUAL _probe_header_major)
          message(FATAL_ERROR
              "FindFFmpeg: header/library VERSION MISMATCH. Headers: "
              "libavcodec ${_probe_header_major}.x; linked library reports "
              "${_probe_lib_major}.x (avcodec_version()=${_probe_output}). "
              "This combination compiles but crashes at runtime (struct "
              "layout drift). Point AVBASE_FFMPEG_ROOT at a build whose "
              "headers and libraries come from the same source, or re-run "
              "with -DAVBASE_SKIP_FFMPEG_PROBE=ON to bypass (at your own risk).")
        endif()
        message(STATUS "FindFFmpeg: header/library probe OK "
                       "(libavcodec major ${_probe_lib_major}, headers agree)")
      endif()
    endif()
  endif()
endif()
