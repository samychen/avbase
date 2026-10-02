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

# Source listing (docs/06 §7.6): unit/base/, unit/media_base/ and unit/player/
# are one directory per target with nothing picked per platform or per option,
# so they are globbed and a new *_unittest.cc is picked up with no edit here.
# unit/media_filters/ is the exception: it holds media_unittests' four files
# beside the three FFmpeg ones that only media_ffmpeg_unittests may compile, so a
# glob there would drag those into the no-ffmpeg gate (G2).

# ---- base ----
file(GLOB AVBASE_BASE_UNITTEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/base/*.cc")
avbase_add_unittest(base_unittests
    SOURCES ${AVBASE_BASE_UNITTEST_SOURCES}
    DEPS avbase::base_test_support)

# ---- media ----
file(GLOB AVBASE_MEDIA_UNITTEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/media_base/*.cc")
avbase_add_unittest(media_unittests
    SOURCES
        ${AVBASE_MEDIA_UNITTEST_SOURCES}
        unit/media_filters/av_sync_controller_unittest.cc
        unit/media_filters/decoder_stream_unittest.cc
        unit/media_filters/retry_data_source_unittest.cc
        unit/media_filters/pipeline_seek_unittest.cc
        unit/media_filters/renderer_impl_unittest.cc
        unit/media_filters/audio_renderer_impl_unittest.cc
        unit/media_filters/synthetic_demuxer_unittest.cc
        unit/media_filters/synthetic_decoders_unittest.cc
        unit/media_filters/video_renderer_impl_unittest.cc
        unit/media_filters/video_frame_compositor_unittest.cc
        unit/media_filters/audio_renderer_algorithm_unittest.cc
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

# ---- media + FFmpeg: real containers, labelled needs-ffmpeg so the
# no-ffmpeg gate (design goal G2) skips them.
if(AVBASE_ENABLE_FFMPEG)
  avbase_add_unittest(media_ffmpeg_unittests
      SOURCES unit/media_filters/ffmpeg_audio_decoder_unittest.cc
              unit/media_filters/ffmpeg_demuxer_unittest.cc
              unit/media_filters/ffmpeg_video_decoder_unittest.cc
              unit/media_filters/pipeline_throttle_unittest.cc
              unit/media_filters/pipeline_track_unittest.cc
      DEPS avbase::platform_ffmpeg avbase::media_test_support
           avbase::base_test_support)
  target_compile_definitions(media_ffmpeg_unittests PRIVATE
      AVBASE_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/tests/testdata")
endif()
