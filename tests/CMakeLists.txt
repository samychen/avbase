include(GoogleTest)

function(avbase_add_unittest name)
  cmake_parse_arguments(PARSE_ARGV 1 T "" "" "SOURCES;DEPS")
  add_executable(${name} ${T_SOURCES})
  # gtest_main carries GTest::gtest in its INTERFACE, and base_test_support
  # carries avbase::base: naming both sides again made ld report "ignoring
  # duplicate libraries" on every test link.
  target_link_libraries(${name} PRIVATE ${T_DEPS} GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
  avbase_configure_target(${name})
  gtest_discover_tests(${name} PROPERTIES TIMEOUT 120)
endfunction()

# ---- shared fakes ----
# Test doubles used by more than one suite: a scripted demuxer stream and the
# resource that hands it out, scriptable decoder factories, the two sinks, and
# the renderer client. One directory, one target, so tests/support/ is globbed
# (docs/06 §7.6). This is its own target rather than part of avbase_media so that
# no fake can ever reach a shipped binary -- the same reason base/ keeps
# avbase_base_test_support separate.
file(GLOB AVBASE_TEST_SUPPORT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/support/*.cc")
add_library(avbase_media_test_support ${AVBASE_TEST_SUPPORT_SOURCES})
add_library(avbase::media_test_support ALIAS avbase_media_test_support)
target_include_directories(avbase_media_test_support PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
target_link_libraries(avbase_media_test_support PUBLIC avbase::media)
avbase_configure_target(avbase_media_test_support)

# Source listing (docs/06 §7.6): every directory under unit/ is globbed now.
# The rule is the one the library directories already follow -- all the .cc in
# a directory must belong to the same target -- so "one directory, one glob"
# holds and a new *_unittest.cc is picked up with no edit here.
#
# unit/media_filters/ used to be the exception, and it was the last explicit
# list in the tree. It held media_unittests' portable suites beside the
# FFmpeg-only ones that only media_ffmpeg_unittests may compile, so a glob
# there would have dragged those into the no-ffmpeg gate (G2) and the
# directory had to be written out file by file. The suites now sit where what
# they link does: the core media ones stay in unit/media_filters/ and land in
# avbase_media, the FFmpeg ones moved to unit/media_ffmpeg/, and the offline
# transcode ones to unit/media_transcode/ beside the avbase_transcode sources.
#
# Membership is by TARGET, not by subject. pipeline_throttle/track/text read
# real containers under tests/testdata/, so they need the FFmpeg target even
# though what they exercise is core media -- splitting by subject would have
# put them in the wrong binary.

if(AVBASE_BUILD_TESTS)
# ---- base ----
file(GLOB AVBASE_BASE_UNITTEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/base/*.cc")
avbase_add_unittest(base_unittests
    SOURCES ${AVBASE_BASE_UNITTEST_SOURCES}
    DEPS avbase::base_test_support)

# ---- media ----
file(GLOB AVBASE_MEDIA_UNITTEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/media_base/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/media_filters/*.cc")
avbase_add_unittest(media_unittests
    SOURCES ${AVBASE_MEDIA_UNITTEST_SOURCES}
    DEPS avbase::media_test_support avbase::base_test_support)
# avbase::media is not named here on purpose: media_test_support carries it in its
# INTERFACE, and naming both sides again makes ld report "ignoring duplicate
# libraries" on this link -- the same lesson the function comment above records
# for base_test_support.

# ---- player: the SDK facade. avbase::player links avbase::media PUBLIC, so the
# media types this suite needs come along without being listed twice.
file(GLOB AVBASE_PLAYER_UNITTEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/player/*.cc")
avbase_add_unittest(player_unittests
    SOURCES ${AVBASE_PLAYER_UNITTEST_SOURCES}
    DEPS avbase::player avbase::base_test_support)

# ---- platform/sdl2: CPU-side pieces of the SDL2 backend (the text
# rasterizer). Runs headless; the GL presentation itself has no window in CI.
if(AVBASE_ENABLE_SDL2)
  file(GLOB AVBASE_SDL2_UNITTEST_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/unit/sdl2/*.cc")
  avbase_add_unittest(sdl2_unittests
      SOURCES ${AVBASE_SDL2_UNITTEST_SOURCES}
      DEPS avbase::platform_sdl2)
endif()

# ---- media + FFmpeg: real containers, labelled needs-ffmpeg so the
# no-ffmpeg gate (design goal G2) skips them.
if(AVBASE_ENABLE_FFMPEG)
  file(GLOB AVBASE_MEDIA_FFMPEG_UNITTEST_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/unit/media_ffmpeg/*.cc")
  avbase_add_unittest(media_ffmpeg_unittests
      SOURCES ${AVBASE_MEDIA_FFMPEG_UNITTEST_SOURCES}
      # avbase::ffmpeg is named here directly. It used to arrive through
      # avbase::transcode, which this target no longer links now that the
      # transcode suites have the binary below to themselves.
      DEPS avbase::ffmpeg avbase::media_test_support
           avbase::base_test_support)
  target_compile_definitions(media_ffmpeg_unittests PRIVATE
      AVBASE_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/tests/testdata")
  # The vendor headers are PRIVATE to avbase_ffmpeg; tests that touch
  # AVFrame fields need them explicitly, and pinned-prefix headers beat
  # whatever /usr/local/include happens to hold.
  target_include_directories(media_ffmpeg_unittests SYSTEM PRIVATE
      ${FFmpeg_INCLUDE_DIRS})

  # ---- offline transcode: its own binary, mirroring avbase_transcode being a
  # target of its own. These suites name media/transcode/ headers only and use
  # no tests/support fixtures, which is why media_test_support is not named --
  # and avbase::ffmpeg is not named either, because avbase::transcode carries
  # it PUBLIC; listing both sides is what makes ld report "ignoring duplicate
  # libraries" (see the helper's header).
  file(GLOB AVBASE_MEDIA_TRANSCODE_UNITTEST_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/unit/media_transcode/*.cc")
  avbase_add_unittest(media_transcode_unittests
      SOURCES ${AVBASE_MEDIA_TRANSCODE_UNITTEST_SOURCES}
      DEPS avbase::transcode avbase::base_test_support)
  target_compile_definitions(media_transcode_unittests PRIVATE
      AVBASE_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/tests/testdata")
  target_include_directories(media_transcode_unittests SYSTEM PRIVATE
      ${FFmpeg_INCLUDE_DIRS})
endif()
endif()

# ---- fuzz targets (Phase 4.5 / G8) -----------------------------------------
# The harness (fuzz_demuxer.cc) is shared by two drivers: the libFuzzer binary
# (coverage-guided, for CI on Linux and long local runs -- needs a clang with
# the libFuzzer runtime, feature-tested below) and a standalone deterministic
# mutator driver (fixed-seed mutations over the seed corpus, no runtime
# dependency, runs under the asan/tsan presets like any test). Corpus seeds
# are real containers under fuzz/seeds/ and grow in-repo.
if(AVBASE_BUILD_FUZZ AND AVBASE_ENABLE_FFMPEG)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  check_cxx_source_compiles("int main(){return 0;}"
      AVBASE_HAVE_LIBFUZZER_RUNTIME)
  unset(CMAKE_REQUIRED_FLAGS)

  add_executable(avbase_fuzz_demuxer fuzz/fuzz_demuxer.cc)
  target_include_directories(avbase_fuzz_demuxer PRIVATE
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(avbase_fuzz_demuxer PRIVATE avbase::ffmpeg)
  target_compile_definitions(avbase_fuzz_demuxer PRIVATE
      AVBASE_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/tests/testdata")
  avbase_configure_target(avbase_fuzz_demuxer)

  # The harness has no main(); the two drivers link it separately. The
  # libFuzzer binary may be EXCLUDE_FROM_ALL (no runtime on this host), so the
  # standalone compiles the source itself rather than sharing an object.
  add_executable(avbase_fuzz_demuxer_standalone
      fuzz/fuzz_standalone_main.cc fuzz/fuzz_demuxer.cc)
  target_include_directories(avbase_fuzz_demuxer_standalone PRIVATE
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(avbase_fuzz_demuxer_standalone PRIVATE
      avbase::ffmpeg)
  target_compile_definitions(avbase_fuzz_demuxer_standalone PRIVATE
      AVBASE_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/tests/testdata"
      AVBASE_SEEDS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/seeds")
  avbase_configure_target(avbase_fuzz_demuxer_standalone)

  if(AVBASE_HAVE_LIBFUZZER_RUNTIME)
    target_compile_options(avbase_fuzz_demuxer PRIVATE -fsanitize=fuzzer)
    target_link_options(avbase_fuzz_demuxer PRIVATE -fsanitize=fuzzer)
  else()
    message(STATUS "libFuzzer runtime unavailable; building the standalone "
                   "fuzz driver only (CI Linux has the runtime)")
    set_target_properties(avbase_fuzz_demuxer PROPERTIES EXCLUDE_FROM_ALL ON)
  endif()
endif()
