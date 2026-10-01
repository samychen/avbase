include(GoogleTest)

function(ijkpp_add_unittest name)
  cmake_parse_arguments(PARSE_ARGV 1 T "" "" "SOURCES;DEPS")
  add_executable(${name} ${T_SOURCES})
  # gtest_main carries GTest::gtest in its INTERFACE, and base_test_support
  # carries ijkpp::base: naming both sides again made ld report "ignoring
  # duplicate libraries" on every test link.
  target_link_libraries(${name} PRIVATE ${T_DEPS} GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
  ijkpp_configure_target(${name})
  gtest_discover_tests(${name} PROPERTIES TIMEOUT 120)
endfunction()

# ---- shared fakes ----
# Test doubles used by more than one suite: a scripted demuxer stream and the
# resource that hands it out, scriptable decoder factories, the two sinks, and
# the renderer client. One directory, one target, so tests/support/ is globbed
# (docs/06 §7.6). This is its own target rather than part of ijkpp_media so that
# no fake can ever reach a shipped binary -- the same reason base/ keeps
# ijkpp_base_test_support separate.
file(GLOB IJKPP_TEST_SUPPORT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/support/*.cc")
add_library(ijkpp_media_test_support ${IJKPP_TEST_SUPPORT_SOURCES})
add_library(ijkpp::media_test_support ALIAS ijkpp_media_test_support)
target_include_directories(ijkpp_media_test_support PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
target_link_libraries(ijkpp_media_test_support PUBLIC ijkpp::media)
ijkpp_configure_target(ijkpp_media_test_support)

# Source listing (docs/06 §7.6): unit/base/, unit/media_base/ and unit/player/
# are one directory per target with nothing picked per platform or per option,
# so they are globbed and a new *_unittest.cc is picked up with no edit here.
# unit/media_filters/ is the exception: it holds media_unittests' four files
# beside the three FFmpeg ones that only media_ffmpeg_unittests may compile, so a
# glob there would drag those into the no-ffmpeg gate (G2).

# ---- base ----
file(GLOB IJKPP_BASE_UNITTEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/base/*.cc")
ijkpp_add_unittest(base_unittests
    SOURCES ${IJKPP_BASE_UNITTEST_SOURCES}
    DEPS ijkpp::base_test_support)

# ---- media ----
file(GLOB IJKPP_MEDIA_UNITTEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/media_base/*.cc")
ijkpp_add_unittest(media_unittests
    SOURCES
        ${IJKPP_MEDIA_UNITTEST_SOURCES}
        unit/media_filters/av_sync_controller_unittest.cc
        unit/media_filters/decoder_stream_unittest.cc
        unit/media_filters/renderer_impl_unittest.cc
        unit/media_filters/audio_renderer_impl_unittest.cc
        unit/media_filters/synthetic_demuxer_unittest.cc
        unit/media_filters/video_renderer_impl_unittest.cc
        unit/media_filters/video_frame_compositor_unittest.cc
        unit/media_filters/audio_renderer_algorithm_unittest.cc
    DEPS ijkpp::media_test_support ijkpp::base_test_support)
# ijkpp::media is not named here on purpose: media_test_support carries it in its
# INTERFACE, and naming both sides again makes ld report "ignoring duplicate
# libraries" on this link -- the same lesson the function comment above records
# for base_test_support.

# ---- player: the SDK facade. ijkpp::player links ijkpp::media PUBLIC, so the
# media types this suite needs come along without being listed twice.
file(GLOB IJKPP_PLAYER_UNITTEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/player/*.cc")
ijkpp_add_unittest(player_unittests
    SOURCES ${IJKPP_PLAYER_UNITTEST_SOURCES}
    DEPS ijkpp::player ijkpp::base_test_support)

# ---- media + FFmpeg: real containers, labelled needs-ffmpeg so the
# no-ffmpeg gate (design goal G2) skips them.
if(IJKPP_ENABLE_FFMPEG)
  ijkpp_add_unittest(media_ffmpeg_unittests
      SOURCES unit/media_filters/ffmpeg_audio_decoder_unittest.cc
              unit/media_filters/ffmpeg_demuxer_unittest.cc
              unit/media_filters/ffmpeg_video_decoder_unittest.cc
      DEPS ijkpp::platform_ffmpeg ijkpp::base_test_support)
  target_compile_definitions(media_ffmpeg_unittests PRIVATE
      IJKPP_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/tests/testdata")
endif()
