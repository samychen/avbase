# Benchmarks. Gated by AVBASE_BUILD_BENCH (default OFF) so the default build
# and the test gate never pay for them. Self-contained: no Google Benchmark and
# no external download — the harness in bench.h is the whole dependency.
#
# avbase::player is linked for the facade entry (BM_EventHub_PostVisit); it
# pulls avbase::media, which the media-layer entries (BM_DecideNextFrame,
# BM_GetMasterClock) need. It costs no new CI dependency: the `full` job already
# builds the entire tree on ubuntu-22.04/24.04 with nothing but ninja and
# python3 installed, so avbase_player, avbase_media and the GL module they pull
# in are known to build there. FFmpeg is still not required — media/ffmpeg is a
# separate target and AVBASE_ENABLE_FFMPEG stays OFF for this one; player is
# written to build without it (every FFmpeg touch is behind #if
# AVBASE_ENABLE_FFMPEG).
if(AVBASE_BUILD_BENCH)
  file(GLOB AVBASE_BENCH_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
  add_executable(avbase_benchmarks ${AVBASE_BENCH_SOURCES})
  target_link_libraries(avbase_benchmarks PRIVATE avbase::base avbase::player)
  target_include_directories(avbase_benchmarks PRIVATE
      ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
  avbase_configure_target(avbase_benchmarks)
endif()
