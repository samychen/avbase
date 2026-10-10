# platform/ — the only place allowed to touch platform and window-system SDKs:
# code that is CHOSEN by the host it runs on. A portable vendor library such as
# FFmpeg does not belong here even though it is a vendor dependency; that one
# lives in media/ffmpeg/, next to the media code that uses it. See docs/09.

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
  # Optional FreeType: the subtitle overlay's CPU rasterizer (text_raster.cc).
  # Without it the overlay slot still works with host-supplied bitmaps; only
  # TextRaster::Rasterize() is compiled out.
  find_package(Freetype QUIET)
  if(Freetype_FOUND)
    target_link_libraries(avbase_platform_sdl2 PRIVATE Freetype::Freetype)
    target_compile_definitions(avbase_platform_sdl2 PUBLIC
        AVBASE_HAVE_FREETYPE=1)
    message(STATUS "FreeType found -- TextRaster enabled")
  else()
    message(STATUS "FreeType not found -- TextRaster compiled out")
  endif()
  avbase_configure_target(avbase_platform_sdl2)
endif()
if(AVBASE_ENABLE_LINUX_NATIVE)
  message(FATAL_ERROR "AVBASE_ENABLE_LINUX_NATIVE is scheduled for milestone M12")
endif()


# The FFmpeg adapter target used to live here. It moved to media/ffmpeg/:
# FFmpeg is not a platform backend but a portable framework, and the
# directory a target lives in has to equal the target (docs/06 §7.6).
# See media/CMakeLists.txt and decision D11 in README.md.
