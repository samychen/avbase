# platform/ — the only place allowed to touch vendor and platform SDKs.
# Populated in M10 (null), M11 (sdl2), M12 (linux). See docs/09.

# SDL2 backend (M11). The window and renderer belong to the host in embedding
# mode; see platform/sdl2/surface.h for the two threading rules.
if(IJKPP_ENABLE_SDL2)
  find_package(SDL2 REQUIRED)
  # One directory, one target -- the option above gates the target, not any file
  # inside it -- so sdl2/ is globbed (docs/06 §7.6) and a new sink needs no edit.
  file(GLOB IJKPP_SDL2_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/sdl2/*.cc")
  add_library(ijkpp_platform_sdl2 ${IJKPP_SDL2_SOURCES})
  add_library(ijkpp::platform_sdl2 ALIAS ijkpp_platform_sdl2)
  target_include_directories(ijkpp_platform_sdl2 PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(ijkpp_platform_sdl2 PUBLIC ijkpp::media SDL2::SDL2)
  target_compile_definitions(ijkpp_platform_sdl2 PRIVATE IJKPP_ENABLE_SDL2=1)
  ijkpp_configure_target(ijkpp_platform_sdl2)
endif()
if(IJKPP_ENABLE_LINUX_NATIVE)
  message(FATAL_ERROR "IJKPP_ENABLE_LINUX_NATIVE is scheduled for milestone M12")
endif()

# FFmpeg adapter layer. This is the ONLY target in the project that links
# FFmpeg; ijkpp_media and ijkpp_base never see a libav* header (design goal G2,
# invariants C4/C8, docs/02 §8).
if(IJKPP_ENABLE_FFMPEG)
  # ffmpeg/ qualifies for a glob: every .cc in it belongs to this one target and
  # none is selected per platform (av_includes.h absorbs version differences).
  # The five ffmpeg_*.cc below are the half that cannot be globbed -- they live
  # in media/filters/ beside the core filters, which is why that directory stays
  # a hand-written list in media/CMakeLists.txt. docs/06 §7.6.
  file(GLOB IJKPP_FFMPEG_DIR_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/*.cc")
  add_library(ijkpp_platform_ffmpeg
      ${IJKPP_FFMPEG_DIR_SOURCES}
      ../media/filters/ffmpeg_glue.cc
      ../media/filters/ffmpeg_demuxer.cc
      ../media/filters/ffmpeg_audio_decoder.cc
      ../media/filters/ffmpeg_video_decoder.cc
      ../media/filters/ffmpeg_decoder_factories.cc)
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
