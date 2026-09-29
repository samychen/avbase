# ijkpp-inspect: diagnostic CLI. Requires FFmpeg because it drives the real
# demuxer and decoders -- a version built on fakes would prove nothing.
#
# Split per subcommand so no file exceeds the 500-line limit (invariant C1) and
# no function exceeds 80 lines (C2). inspect_common holds the plumbing all three
# share: argument parsing, the demuxer Host, and the task-queue Pump.
add_executable(ijkpp-inspect
    inspect_main.cc
    inspect_common.cc
    inspect_probe.cc
    inspect_decode.cc
    inspect_sync.cc)
target_link_libraries(ijkpp-inspect PRIVATE
    ijkpp::platform_ffmpeg
    ijkpp::base_test_support)
target_include_directories(ijkpp-inspect PRIVATE ${CMAKE_SOURCE_DIR})
ijkpp_configure_target(ijkpp-inspect)
