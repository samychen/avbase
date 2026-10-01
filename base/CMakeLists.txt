# base/ — the Chromium-shaped foundation layer. Zero external dependencies:
# this target must build and its tests must pass on a machine with nothing but
# a C++20 compiler (design goal G2).
#
# Source listing stays explicit (docs/06 §7.6): this is the layer whose
# abstraction is one file per platform -- threading/platform_thread_posix.cc
# today, the posix/win pairs docs/06 §7.1 plans for synchronization/ later -- so
# directory membership is not target membership, the same reason media/filters/
# is hand-written. A glob here would compile the wrong platform's file.
add_library(ijkpp_base
    logging.cc
    check.cc
    synchronization/lock.cc
    task/task_runner.cc
    task/sequenced_task_runner.cc
    task/task_queue.cc
    threading/platform_thread_posix.cc
    threading/thread.cc
    time/time.cc
    time/default_tick_clock.cc)
add_library(ijkpp::base ALIAS ijkpp_base)

target_include_directories(ijkpp_base PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ijkpp>)
target_link_libraries(ijkpp_base PUBLIC Threads::Threads)
target_compile_definitions(ijkpp_base PRIVATE IJKPP_BASE_IMPLEMENTATION=1)
ijkpp_configure_target(ijkpp_base)

# Test-only support library, kept separate from ijkpp_base so it can never
# reach a shipped binary.
if(IJKPP_BUILD_TESTS)
  add_library(ijkpp_base_test_support test/task_environment.cc)
  add_library(ijkpp::base_test_support ALIAS ijkpp_base_test_support)
  target_include_directories(ijkpp_base_test_support PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(ijkpp_base_test_support PUBLIC ijkpp::base)
  target_compile_definitions(ijkpp_base_test_support PRIVATE
      IJKPP_BASE_IMPLEMENTATION=1)
  ijkpp_configure_target(ijkpp_base_test_support)
endif()
