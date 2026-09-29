# Copyright 2026 The ijkpp Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Turns the architecture constraints in docs/02 §2.1 into a build target, so
# that they cannot silently rot. See docs/06 §9.

find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_Interpreter_FOUND)
  add_custom_target(check-invariants
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_invariants.py
              --root ${CMAKE_SOURCE_DIR}
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Checking architecture & style invariants (C1-C19)")

  if(IJKPP_IS_TOP_LEVEL)
    add_custom_target(ci-quick DEPENDS check-invariants)
  endif()
else()
  message(STATUS "Python3 not found; check-invariants target disabled")
endif()
