# media/ — media framework. media/base holds interfaces and value types,
# media/filters holds implementations. Neither links FFmpeg (that is
# ijkpp_platform_ffmpeg); see docs/02 §8 and decision D11.
add_library(ijkpp_media
    base/audio_buffer.cc
    base/audio_bus.cc
    base/audio_decoder.cc
    base/audio_parameters.cc
    base/data_source.cc
    base/data_source_descriptor.cc
    base/decoder_buffer.cc
    base/decoder_buffer_queue.cc
    base/decoder_config.cc
    base/demuxer.cc
    base/decoder_status.cc
    base/media_error.cc
    base/media_info.cc
    base/media_log.cc
    base/media_types.cc
    base/native_display.cc
    base/video_decoder.cc
    base/video_frame.cc
    base/video_frame_queue.cc
    base/waiting.cc
    filters/av_sync_controller.cc
    filters/clock.cc
    filters/decoder_selector.cc
    filters/decoder_stream.cc
    filters/video_frame_compositor.cc)

# FFmpeg-backed demuxer. Compiled into ijkpp_platform_ffmpeg, never into
# ijkpp_media, so that the core target carries no FFmpeg dependency (G2).
add_library(ijkpp::media ALIAS ijkpp_media)

target_include_directories(ijkpp_media PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ijkpp>)
target_link_libraries(ijkpp_media PUBLIC ijkpp::base)
target_compile_definitions(ijkpp_media PRIVATE IJKPP_MEDIA_IMPLEMENTATION=1)
ijkpp_configure_target(ijkpp_media)
