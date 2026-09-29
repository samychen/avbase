# Copyright 2026 The ijkpp Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

find_package(GTest QUIET)
if(NOT GTest_FOUND)
  # Debian/Ubuntu ship libgtest.a without a CMake package file.
  find_library(IJKPP_GTEST_LIB  NAMES gtest)
  find_library(IJKPP_GMOCK_LIB  NAMES gmock)
  find_library(IJKPP_GTEST_MAIN NAMES gtest_main)
  find_path(IJKPP_GTEST_INCLUDE NAMES gtest/gtest.h)
  if(IJKPP_GTEST_LIB AND IJKPP_GTEST_INCLUDE)
    add_library(GTest::gtest UNKNOWN IMPORTED)
    set_target_properties(GTest::gtest PROPERTIES
        IMPORTED_LOCATION "${IJKPP_GTEST_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${IJKPP_GTEST_INCLUDE}"
        INTERFACE_LINK_LIBRARIES "Threads::Threads")
    if(IJKPP_GTEST_MAIN)
      add_library(GTest::gtest_main UNKNOWN IMPORTED)
      set_target_properties(GTest::gtest_main PROPERTIES
          IMPORTED_LOCATION "${IJKPP_GTEST_MAIN}"
          INTERFACE_LINK_LIBRARIES "GTest::gtest")
    endif()
    if(IJKPP_GMOCK_LIB)
      add_library(GTest::gmock UNKNOWN IMPORTED)
      set_target_properties(GTest::gmock PROPERTIES
          IMPORTED_LOCATION "${IJKPP_GMOCK_LIB}"
          INTERFACE_LINK_LIBRARIES "GTest::gtest")
    endif()
    set(GTest_FOUND TRUE)
  endif()
endif()

if(NOT GTest_FOUND)
  message(FATAL_ERROR
      "GoogleTest not found. Install libgtest-dev/libgmock-dev, or configure "
      "with -DIJKPP_BUILD_TESTS=OFF.")
endif()
