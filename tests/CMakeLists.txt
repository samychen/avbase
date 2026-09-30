include(GoogleTest)

function(ijkpp_add_unittest name)
  cmake_parse_arguments(PARSE_ARGV 1 T "" "" "SOURCES;DEPS")
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE ${T_DEPS} GTest::gtest GTest::gtest_main)
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
  ijkpp_configure_target(${name})
  gtest_discover_tests(${name} PROPERTIES TIMEOUT 120)
endfunction()

# ---- base ----
ijkpp_add_unittest(base_unittests
    SOURCES
        unit/base/time_unittest.cc
        unit/base/expected_unittest.cc
        unit/base/scoped_refptr_unittest.cc
        unit/base/refcount_ownership_unittest.cc It is marked STATUS: DRAFT, and the project's own rule
        # for DRAFT files (docs/PROGRESS.md, fifth round; repeated in the file
        # headers added in the ninth) is that they are excluded from every CMake
        # target, because a file that cannot compile must not be reachable from
        # a build. It was registered here by mistake in the ninth round, which
        # would have failed the next real build. Add it to this list the first
        # time it compiles, and drop the DRAFT banner from the file at the same
        # moment.
        unit/base/weak_ptr_unittest.cc
        unit/base/callback_bind_unittest.cc
        unit/base/synchronization_unittest.cc
        unit/base/observer_list_unittest.cc
        unit/base/task_queue_unittest.cc
        unit/base/thread_unittest.cc
        unit/base/task_environment_unittest.cc
    DEPS ijkpp::base ijkpp::base_test_support)

# ---- media ----
ijkpp_add_unittest(media_unittests
    SOURCES
        unit/media_base/audio_buffer_unittest.cc
        unit/media_base/decoder_buffer_unittest.cc
        unit/media_base/decoder_buffer_queue_unittest.cc
        unit/media_base/media_log_unittest.cc
        unit/media_base/video_frame_queue_unittest.cc
        unit/media_base/video_frame_unittest.cc
        unit/media_filters/av_sync_controller_unittest.cc
        unit/media_filters/decoder_stream_unittest.cc
        unit/media_filters/video_frame_compositor_unittest.cc
        unit/media_filters/audio_renderer_algorithm_unittest.cc
    DEPS ijkpp::media ijkpp::base_test_support)

# ---- player: the SDK facade. ijkpp::player links ijkpp::media PUBLIC, so the
# media types this suite needs come along without being listed twice.
ijkpp_add_unittest(player_unittests
    SOURCES
        unit/player/deps_ownership_unittest.cc
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
