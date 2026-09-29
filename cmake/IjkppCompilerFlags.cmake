# Copyright 2026 The ijkpp Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)

if(IJKPP_BUILD_SHARED)
  if(WIN32)
    set(IJKPP_DLLEXPORT "__declspec(dllexport)")
    set(IJKPP_DLLIMPORT "__declspec(dllimport)")
  else()
    set(IJKPP_DLLEXPORT "__attribute__((visibility(\"default\")))")
    set(IJKPP_DLLIMPORT "__attribute__((visibility(\"default\")))")
  endif()
else()
  set(IJKPP_DLLEXPORT "")
  set(IJKPP_DLLIMPORT "")
endif()

set(IJKPP_WARNINGS_COMMON
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wunused
    -Wcast-align -Wnull-dereference -Wdouble-promotion
    -Wimplicit-fallthrough -Wformat=2
    -Werror=return-type -Werror=uninitialized -Werror=parentheses
    -Werror=narrowing -Werror=delete-non-virtual-dtor -Werror=reorder
    -Wno-unknown-pragmas)
set(IJKPP_WARNINGS_STRICT
    -Wconversion -Wsign-conversion -Wold-style-cast -Wuseless-cast
    -Wsuggest-override -Wzero-as-null-pointer-constant -Wextra-semi
    -Wpessimizing-move)

function(ijkpp_configure_target target)
  target_compile_features(${target} PUBLIC cxx_std_20)

  # Google / Chromium: exceptions and RTTI are disabled project-wide.
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
    target_compile_definitions(${target} PRIVATE IJKPP_NO_EXCEPTIONS=1)
  else()
    target_compile_options(${target} PRIVATE /GR- /EHs-c-)
    target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus
        /wd4251 $<$<BOOL:${IJKPP_WERROR}>:/WX>)
  else()
    target_compile_options(${target} PRIVATE ${IJKPP_WARNINGS_COMMON}
        $<$<BOOL:${IJKPP_STRICT_WARNINGS}>:${IJKPP_WARNINGS_STRICT}>
        $<$<BOOL:${IJKPP_WERROR}>:-Werror>)
    check_cxx_compiler_flag(-Wthread-safety IJKPP_HAS_THREAD_SAFETY)
    if(IJKPP_HAS_THREAD_SAFETY)
      target_compile_options(${target} PRIVATE -Wthread-safety)
      target_compile_definitions(${target} PRIVATE IJKPP_THREAD_SAFETY_ANALYSIS=1)
    endif()
  endif()

  target_compile_definitions(${target} PRIVATE
      $<$<BOOL:${IJKPP_ENABLE_DCHECK}>:IJKPP_ENABLE_DCHECK=1>
      $<$<CONFIG:Debug>:IJKPP_DEBUG=1>)

  if(IJKPP_SANITIZERS)
    set(_sf "")
    foreach(s IN LISTS IJKPP_SANITIZERS)
      list(APPEND _sf "-fsanitize=${s}")
    endforeach()
    target_compile_options(${target} PRIVATE ${_sf} -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE ${_sf})
  endif()

  if(IJKPP_COVERAGE)
    target_compile_options(${target} PRIVATE --coverage -O0 -g)
    target_link_options(${target} PRIVATE --coverage)
  endif()

  target_include_directories(${target} SYSTEM PRIVATE
      "${CMAKE_BINARY_DIR}/generated")
endfunction()

check_cxx_source_compiles("
  #include <expected>
  int main() { std::expected<int, int> e{1}; return *e - 1; }"
  IJKPP_HAVE_STD_EXPECTED)

if(IJKPP_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT IJKPP_IPO_OK OUTPUT IJKPP_IPO_MSG)
  if(IJKPP_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  else()
    message(WARNING "LTO requested but unsupported: ${IJKPP_IPO_MSG}")
  endif()
endif()
