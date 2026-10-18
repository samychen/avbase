# Benchmarks. Gated by AVBASE_BUILD_BENCH (default OFF) so the default build
# and the test gate never pay for them. Self-contained: only depends on
# avbase::base, no Google Benchmark, no external download.
if(AVBASE_BUILD_BENCH)
  file(GLOB AVBASE_BENCH_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
  add_executable(avbase_benchmarks ${AVBASE_BENCH_SOURCES})
  target_link_libraries(avbase_benchmarks PRIVATE avbase::base)
  target_include_directories(avbase_benchmarks PRIVATE
      ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
  avbase_configure_target(avbase_benchmarks)
endif()
