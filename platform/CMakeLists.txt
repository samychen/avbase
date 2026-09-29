# platform/ — the only place allowed to touch vendor and platform SDKs.
# Populated in M10 (null), M11 (sdl2), M12 (linux). See docs/09.

if(IJKPP_ENABLE_SDL2)
  message(FATAL_ERROR "IJKPP_ENABLE_SDL2 is scheduled for milestone M11")
endif()
if(IJKPP_ENABLE_LINUX_NATIVE)
  message(FATAL_ERROR "IJKPP_ENABLE_LINUX_NATIVE is scheduled for milestone M12")
endif()

# FFmpeg adapter layer. This is the ONLY target in the project that links
# FFmpeg; ijkpp_media and ijkpp_base never see a libav* header (design goal G2,
# invariants C4/C8, docs/02 §8).
if(IJKPP_ENABLE_FFMPEG)
  add_library(ijkpp_platform_ffmpeg
      ffmpeg/compat.cc
      ffmpeg/av_packet_storage.cc
      ffmpeg/log_bridge.cc
      ffmpeg/interrupt_callback.cc
      ../media/filters/ffmpeg_glue.cc
      ../media/filters/ffmpeg_demuxer.cc
      ../media/filters/ffmpeg_audio_decoder.cc
      ../media/filters/ffmpeg_video_decoder.cc)
  add_library(ijkpp::platform_ffmpeg ALIAS ijkpp_platform_ffmpeg)

  target_include_directories(ijkpp_platform_ffmpeg PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(ijkpp_platform_ffmpeg
      PUBLIC  ijkpp::media
      PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil
              FFmpeg::swscale FFmpeg::swresample)
  target_compile_definitions(ijkpp_platform_ffmpeg PRIVATE IJKPP_ENABLE_FFMPEG=1)
  ijkpp_configure_target(ijkpp_platform_ffmpeg)
  # One precompiled header for the whole vendor layer: av_includes.h is the
  # single extern "C" wrap point, so every TU here needs it anyway.
  target_precompile_headers(ijkpp_platform_ffmpeg PRIVATE
      "${CMAKE_SOURCE_DIR}/platform/ffmpeg/av_includes.h")
endif()
