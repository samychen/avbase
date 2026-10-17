# player/public/c/ -- the C ABI compatibility layer (docs/08 M15).
#
# A SEPARATE library, not a source file added to avbase_player. The reason is
# the consumer: a C or JNI host links this target and must not acquire the C++
# headers or the C++ ABI as a dependency. Making it part of avbase_player would
# force exactly that, and the C ABI would stop being optional in practice.
#
# Enabled by -DAVBASE_ENABLE_CAPI=ON; off by default because a project that only
# uses the C++ API should not pay for it.
# One .cc per surface (player / display / events): the whole C ABI in one file
# runs past the C1 500-line limit, and the split follows the three responsibilities
# rather than being an arbitrary chop. avbase_c_internal.h holds what they share.
add_library(avbase_cabi STATIC
    avbase_c_player.cc
    avbase_c_display.cc
    avbase_c_events.cc)

target_include_directories(avbase_cabi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/avbase>)

target_compile_definitions(avbase_cabi PRIVATE AVBASE_C_IMPLEMENTATION=1)
# PUBLIC: the header is the interface, so a consumer of this target needs the
# include path to find avbase_c.h itself.
target_link_libraries(avbase_cabi PUBLIC avbase::player)
avbase_configure_target(avbase_cabi)

# The avbase:: spelling every other target in the tree carries (avbase::player,
# avbase::media, avbase::ffmpeg, avbase::gl). Without it a consumer inside this
# build can only say avbase_cabi, and the first in-tree consumer -- the smoke
# test below -- is exactly what fails, at configure time, with a message that
# points at the typo rather than at the missing alias.
add_library(avbase::cabi ALIAS avbase_cabi)

# Installed as its own header so a C host can be handed ONLY this file plus a
# platform's window handle -- not the whole C++ public surface.
install(FILES avbase_c.h avbase_c_export.h
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/avbase/player/public/c")

# ---- the C-side smoke test (docs/08 M15 acceptance) ----
#
# It exists as a build target rather than as a shell script that compiles it,
# for three reasons that each cost something when they are not true:
#
#  1. THE COMPILE IS THE TEST. What can break is avbase_c.h ceasing to be valid
#     C. Only a C compiler proves that, and CMAKE_C_STANDARD 99 + -Werror make
#     it a gate rather than a one-off command somebody remembers to run.
#  2. THE SCRIPT WAS macOS-ONLY. It carried -mmacosx-version-min plus seven
#     hardcoded paths into tools/build/lib for the pinned FFmpeg dylibs, so it
#     could not run on the Linux CI runners at all -- meaning the C ABI had no
#     gate anywhere but one developer's machine. CMake resolves the link line
#     from the targets instead, which is what makes the same file portable.
#  3. CTest makes it blocking in the same way as every other test, so a
#     regression is reported by CI rather than discovered by a person.
#
# avbase_configure_target() is applied on purpose even though this is a C
# target: verified by experiment (a throwaway project, not by reading) that the
# full flag set -- cxx_std_20, -fno-exceptions -fno-rtti, the -Wpedantic
# warning block, -Werror -- compiles a C99 translation unit cleanly, because
# clang and GCC accept those warnings for C or ignore them. What CMake does
# choose automatically, and correctly, is the C++ LINK driver: a target with
# only C sources would otherwise link with `cc` and fail on libc++ symbols,
# which is the same wall tools/check_capi.sh used to paper over by invoking c++
# by hand.
#
# The media argument is the test's own business (it needs a real file to reach
# kCompleted), so it is passed here rather than hardcoded to a path that only
# exists in one developer's build tree.
if(AVBASE_BUILD_TESTS)
  add_executable(avbase_capi_smoke capi_smoke_test.c)
  target_link_libraries(avbase_capi_smoke PRIVATE avbase::cabi)
  target_include_directories(avbase_capi_smoke PRIVATE ${CMAKE_SOURCE_DIR})
  avbase_configure_target(avbase_capi_smoke)
  # C99 rather than the project-wide C++20: the header promises C consumers,
  # and a dialect newer than the promise would compile things the promise does
  # not cover.
  set_target_properties(avbase_capi_smoke PROPERTIES
      C_STANDARD 99
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF)
  # No find_package(FFmpeg) here, unlike tools/inspect: avbase_cabi links
  # avbase::player PUBLIC, and avbase_player already links avbase::ffmpeg PUBLIC
  # when FFmpeg is on, so the dependency arrives with its include paths. Naming
  # it a second time is what produced
  #   ld: warning: ignoring duplicate libraries: 'lib/libavbase_ffmpeg.a'
  # -- a warning that trains people to ignore linker output.
  add_test(NAME capi_smoke_test
           COMMAND avbase_capi_smoke
                   "${CMAKE_SOURCE_DIR}/tests/testdata/small_h264_aac_3s.mp4")
  set_tests_properties(capi_smoke_test PROPERTIES TIMEOUT 120)
endif()