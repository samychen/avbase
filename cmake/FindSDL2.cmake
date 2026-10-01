# Copyright 2026 The ijkpp Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# FindSDL2 — locate the SDL2 development headers and library.
#
# Tries, in order:
#   1. An SDL2_ROOT / SDL2DIR prefix (the -D<TYPE>_ROOT pattern CMake users
#      expect), via find_path/find_library.
#   2. Plain find_path/find_library over the system paths, which on Homebrew
#      lands in /opt/homebrew (headers in the SDL2/ subdirectory).
# Produces an imported target SDL2::SDL2.

find_path(SDL2_INCLUDE_DIR
    NAMES SDL.h
    PATH_SUFFIXES SDL2 SDL2/include include/SDL2
    HINTS ${SDL2_ROOT} ${SDL2DIR}
    PATHS /opt/homebrew /usr/local /usr)
find_library(SDL2_LIBRARY
    NAMES SDL2 SDL2-2.0
    HINTS ${SDL2_ROOT} ${SDL2DIR}
    PATHS /opt/homebrew /usr/local /usr
    PATH_SUFFIXES lib lib64)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SDL2
    REQUIRED_VARS SDL2_LIBRARY SDL2_INCLUDE_DIR)

if(SDL2_FOUND AND NOT TARGET SDL2::SDL2)
  add_library(SDL2::SDL2 UNKNOWN IMPORTED)
  set_target_properties(SDL2::SDL2 PROPERTIES
      IMPORTED_LOCATION "${SDL2_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIR}")
endif()
