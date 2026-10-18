# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)

if(AVBASE_BUILD_SHARED)
  if(WIN32)
    set(AVBASE_DLLEXPORT "__declspec(dllexport)")
    set(AVBASE_DLLIMPORT "__declspec(dllimport)")
  else()
    set(AVBASE_DLLEXPORT "__attribute__((visibility(\"default\")))")
    set(AVBASE_DLLIMPORT "__attribute__((visibility(\"default\")))")
  endif()
else()
  set(AVBASE_DLLEXPORT "")
  set(AVBASE_DLLIMPORT "")
endif()

set(AVBASE_WARNINGS_COMMON
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wunused
    -Wcast-align -Wnull-dereference -Wdouble-promotion
    -Wimplicit-fallthrough -Wformat=2
    -Werror=return-type -Werror=uninitialized -Werror=parentheses
    -Werror=narrowing -Werror=delete-non-virtual-dtor -Werror=reorder
    -Wno-unknown-pragmas)
set(AVBASE_WARNINGS_STRICT
    -Wconversion -Wsign-conversion -Wold-style-cast
    -Wsuggest-override -Wzero-as-null-pointer-constant -Wextra-semi
    -Wpessimizing-move)
# GCC-only. clang rejects -Wuseless-cast as an unknown option, which together
# with AVBASE_WERROR makes the strict/debug preset -- the configuration
# docs/BUILDING.md points reviewers at -- fail on the first translation unit
# instead of compiling the tree. There is no clang equivalent to fall back to.
set(AVBASE_WARNINGS_STRICT_GCC_ONLY
    -Wuseless-cast)

# The ABI half of avbase_configure_target(): every macro that changes how a
# type in a PUBLIC HEADER is laid out.
#
# This is separate from the function below because a host application cannot
# always take the library's -fno-exceptions / -fno-rtti (Qt's meta-object
# system needs RTTI), but it MUST still see identical layouts. Getting this
# wrong is silent and expensive: SEQUENCE_CHECKER expands to a member when
# AVBASE_ENABLE_DCHECK is defined and to nothing when it is not, so a
# VideoFrame built by the library (one layout) and released through an inline
# scoped_refptr in the app (the other layout) AddRef()s the wrong field -- the
# refcount never grows, the frame is freed while the compositor still holds it,
# and the crash lands in heap-allocator internals somewhere unrelated.
#
# Measured on apps/player_qt: AddressSanitizer reported heap-use-after-free in
# VideoFrameCompositor::Render, 40 bytes into a 232-byte VideoFrame, freed from
# the app's paint path. Adding these definitions fixed it.
function(avbase_configure_abi_definitions target)
  target_compile_definitions(${target} PRIVATE
      $<$<BOOL:${AVBASE_ENABLE_DCHECK}>:AVBASE_ENABLE_DCHECK=1>
      $<$<CONFIG:Debug>:AVBASE_DEBUG=1>)
  check_cxx_compiler_flag(-Wthread-safety _avbase_has_thread_safety)
  if(_avbase_has_thread_safety)
    target_compile_definitions(${target} PRIVATE
        AVBASE_THREAD_SAFETY_ANALYSIS=1)
  endif()
endfunction()

function(avbase_configure_target target)
  target_compile_features(${target} PUBLIC cxx_std_20)

  # Google / Chromium: exceptions and RTTI are disabled project-wide.
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
    target_compile_definitions(${target} PRIVATE AVBASE_NO_EXCEPTIONS=1)
  else()
    target_compile_options(${target} PRIVATE /GR- /EHs-c-)
    target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus
        /wd4251 $<$<BOOL:${AVBASE_WERROR}>:/WX>)
  else()
    target_compile_options(${target} PRIVATE ${AVBASE_WARNINGS_COMMON}
        $<$<BOOL:${AVBASE_STRICT_WARNINGS}>:${AVBASE_WARNINGS_STRICT}>
        $<$<AND:$<BOOL:${AVBASE_STRICT_WARNINGS}>,$<CXX_COMPILER_ID:GNU>>:${AVBASE_WARNINGS_STRICT_GCC_ONLY}>
        $<$<BOOL:${AVBASE_WERROR}>:-Werror>)
    check_cxx_compiler_flag(-Wthread-safety AVBASE_HAS_THREAD_SAFETY)
    if(AVBASE_HAS_THREAD_SAFETY)
      target_compile_options(${target} PRIVATE -Wthread-safety)
    endif()
  endif()

  avbase_configure_abi_definitions(${target})

  if(AVBASE_SANITIZERS)
    set(_sf "")
    foreach(s IN LISTS AVBASE_SANITIZERS)
      list(APPEND _sf "-fsanitize=${s}")
    endforeach()
    target_compile_options(${target} PRIVATE ${_sf} -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE ${_sf})
  endif()

  if(AVBASE_COVERAGE)
    target_compile_options(${target} PRIVATE --coverage -O0 -g)
    target_link_options(${target} PRIVATE --coverage)
  endif()

  target_include_directories(${target} SYSTEM PRIVATE
      "${CMAKE_BINARY_DIR}/generated")
endfunction()

check_cxx_source_compiles("
  #include <expected>
  int main() { std::expected<int, int> e{1}; return *e - 1; }"
  AVBASE_HAVE_STD_EXPECTED)

if(AVBASE_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT AVBASE_IPO_OK OUTPUT AVBASE_IPO_MSG)
  if(AVBASE_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  else()
    message(WARNING "LTO requested but unsupported: ${AVBASE_IPO_MSG}")
  endif()
endif()
