# platform/ — the only place allowed to touch vendor and platform SDKs.
# Populated in M10 (null), M11 (sdl2), M12 (linux). See docs/09.

# SDL2 backend (M11). The window and renderer belong to the host in embedding
# mode; see platform/sdl2/surface.h for the two threading rules.
if(AVBASE_ENABLE_SDL2)
  find_package(SDL2 REQUIRED)
  # One directory, one target -- the option above gates the target, not any file
  # inside it -- so sdl2/ is globbed (docs/06 §7.6) and a new sink needs no edit.
  file(GLOB AVBASE_SDL2_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/sdl2/*.cc")
  add_library(avbase_platform_sdl2 ${AVBASE_SDL2_SOURCES})
  add_library(avbase::platform_sdl2 ALIAS avbase_platform_sdl2)
  target_include_directories(avbase_platform_sdl2 PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(avbase_platform_sdl2 PUBLIC avbase::media SDL2::SDL2)
  target_compile_definitions(avbase_platform_sdl2 PRIVATE AVBASE_ENABLE_SDL2=1)
  avbase_configure_target(avbase_platform_sdl2)
endif()
if(AVBASE_ENABLE_LINUX_NATIVE)
  message(FATAL_ERROR "AVBASE_ENABLE_LINUX_NATIVE is scheduled for milestone M12")
endif()

# Hardware decoder factories (Phase 3). All three instantiate the generic
# FFmpegHwVideoDecoder inside avbase_platform_ffmpeg -- invariant C8 keeps the
# libav includes in that one target, so the hwaccel paths live there too --
# and only differ in device name, NativeHandle kind and codec list. Three
# explicit blocks rather than a loop: check_invariants C25 resolves globs
# statically, so the directory names must be literal in this file.
if(AVBASE_ENABLE_VIDEOTOOLBOX)
  if(NOT AVBASE_ENABLE_FFMPEG)
    message(FATAL_ERROR "AVBASE_ENABLE_VIDEOTOOLBOX requires AVBASE_ENABLE_FFMPEG")
  endif()
  file(GLOB AVBASE_HW_VIDEOTOOLBOX_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/videotoolbox/*.cc")
  add_library(avbase_platform_videotoolbox ${AVBASE_HW_VIDEOTOOLBOX_SOURCES})
  add_library(avbase::platform_videotoolbox ALIAS avbase_platform_videotoolbox)
  target_include_directories(avbase_platform_videotoolbox PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(avbase_platform_videotoolbox PUBLIC
      avbase::platform_ffmpeg)
  avbase_configure_target(avbase_platform_videotoolbox)
endif()
if(AVBASE_ENABLE_VAAPI)
  if(NOT AVBASE_ENABLE_FFMPEG)
    message(FATAL_ERROR "AVBASE_ENABLE_VAAPI requires AVBASE_ENABLE_FFMPEG")
  endif()
  file(GLOB AVBASE_HW_VAAPI_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/vaapi/*.cc")
  add_library(avbase_platform_vaapi ${AVBASE_HW_VAAPI_SOURCES})
  add_library(avbase::platform_vaapi ALIAS avbase_platform_vaapi)
  target_include_directories(avbase_platform_vaapi PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(avbase_platform_vaapi PUBLIC avbase::platform_ffmpeg)
  avbase_configure_target(avbase_platform_vaapi)
endif()
if(AVBASE_ENABLE_D3D11)
  if(NOT AVBASE_ENABLE_FFMPEG)
    message(FATAL_ERROR "AVBASE_ENABLE_D3D11 requires AVBASE_ENABLE_FFMPEG")
  endif()
  file(GLOB AVBASE_HW_D3D11_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/d3d11/*.cc")
  add_library(avbase_platform_d3d11 ${AVBASE_HW_D3D11_SOURCES})
  add_library(avbase::platform_d3d11 ALIAS avbase_platform_d3d11)
  target_include_directories(avbase_platform_d3d11 PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(avbase_platform_d3d11 PUBLIC avbase::platform_ffmpeg)
  avbase_configure_target(avbase_platform_d3d11)
endif()

# FFmpeg adapter layer. This is the ONLY target in the project that links
# FFmpeg; avbase_media and avbase_base never see a libav* header (design goal G2,
# invariants C4/C8, docs/02 §8).
if(AVBASE_ENABLE_FFMPEG)
  # ffmpeg/ qualifies for a glob: every .cc in it belongs to this one target and
  # none is selected per platform (av_includes.h absorbs version differences).
  # The five ffmpeg_*.cc below are the half that cannot be globbed -- they live
  # in media/filters/ beside the core filters, which is why that directory stays
  # a hand-written list in media/CMakeLists.txt. docs/06 §7.6.
  file(GLOB AVBASE_FFMPEG_DIR_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/*.cc")
  add_library(avbase_platform_ffmpeg
      ${AVBASE_FFMPEG_DIR_SOURCES}
      ../media/filters/ffmpeg_glue.cc
      ../media/filters/ffmpeg_demuxer.cc
      ../media/filters/ffmpeg_audio_decoder.cc
      ../media/filters/ffmpeg_video_decoder.cc
      ../media/filters/ffmpeg_decoder_factories.cc
      ../media/filters/ffmpeg_text_decoder.cc
      ffmpeg/data_source_io.cc)
  add_library(avbase::platform_ffmpeg ALIAS avbase_platform_ffmpeg)

  target_include_directories(avbase_platform_ffmpeg PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(avbase_platform_ffmpeg
      PUBLIC  avbase::media
      PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil
              FFmpeg::swscale FFmpeg::swresample)
  target_compile_definitions(avbase_platform_ffmpeg PRIVATE AVBASE_ENABLE_FFMPEG=1)
  avbase_configure_target(avbase_platform_ffmpeg)
  # One precompiled header for the whole vendor layer: av_includes.h is the
  # single extern "C" wrap point, so every TU here needs it anyway.
  target_precompile_headers(avbase_platform_ffmpeg PRIVATE
      "${CMAKE_SOURCE_DIR}/platform/ffmpeg/av_includes.h")
endif()
