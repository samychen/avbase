# platform/ — the only place allowed to touch platform and window-system SDKs:
# code that is CHOSEN by the host it runs on. A portable vendor library such as
# FFmpeg does not belong here even though it is a vendor dependency; that one
# lives in media/ffmpeg/, next to the media code that uses it -- see
# docs/archive/09 for the decision that moved it there.

# The shared GL rendering layer used to live here as platform/gl/. It moved to
# render/gl/ (target avbase::gl): this directory holds what the HOST
# SELECTS, and that layer is chosen by nobody -- every backend links it and it
# carries no window-system SDK at all. See render/CMakeLists.txt and docs/02 §6.

# Hardware texture import: the platform half of the zero-copy path, and a
# target of its own because it is an axis of its own. CGL/IOSurface, EGLImage
# and WGL_NV_DX_interop are chosen by the HOST'S GRAPHICS STACK, not by its
# window system -- the same CGL backend serves the SDL2, GLFW and EGL sinks --
# so filing the implementation inside sdl2/ or glfw/ would make three copies of
# it and a fourth directory combination nobody can build (docs/02 §6).
#
# Defined before the backends because they all link it; each one calls
# CreateHwTextureImporter() and none of them learns which API its host has.
# The interface it implements lives in media/gl/gl_hw_texture_importer.h, which
# is where the shape can live and the implementation cannot: invariant C5 keeps
# OpenGL/CoreVideo headers out of media/.
add_library(avbase_platform_hwimport
    "${CMAKE_CURRENT_SOURCE_DIR}/hwimport/hw_importer_factory.cc")
add_library(avbase::platform_hwimport ALIAS avbase_platform_hwimport)
target_include_directories(avbase_platform_hwimport PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
target_link_libraries(avbase_platform_hwimport PUBLIC avbase::media)
if(APPLE)
  target_sources(avbase_platform_hwimport PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/hwimport/cgl_iosurface_importer.cc")
  # OpenGL for CGLTexImageIOSurface2D, CoreVideo for CVPixelBuffer, IOSurface
  # because that is what a CVPixelBuffer hands out on macOS.
  target_link_libraries(avbase_platform_hwimport PRIVATE
      "-framework OpenGL" "-framework CoreVideo" "-framework IOSurface"
      "-framework CoreFoundation")
endif()
avbase_configure_target(avbase_platform_hwimport)

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
  target_link_libraries(avbase_platform_sdl2 PUBLIC avbase::media avbase::gl SDL2::SDL2)
  # The zero-copy import path; the sink builds one and hands it to the presenter.
  target_link_libraries(avbase_platform_sdl2 PRIVATE avbase::platform_hwimport)
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

# GLFW backend: OPTIONAL. The host owns the GLFWwindow and its GL context; this
# backend only adapts them to NativeGLContext. Enable with -DAVBASE_ENABLE_GLFW=ON.
# Skipped (warning, no FATAL) when glfw3 is not installed, so a host without
# GLFW still builds the rest of the tree. The glob below lets check_invariants
# (C25) see the sources even when this backend is off.
option(AVBASE_ENABLE_GLFW "Build the GLFW GL backend" OFF)
if(AVBASE_ENABLE_GLFW)
  find_package(glfw3 QUIET)
  if(glfw3_FOUND)
    file(GLOB AVBASE_GLFW_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/glfw/*.cc")
    add_library(avbase_platform_glfw ${AVBASE_GLFW_SOURCES})
    add_library(avbase::platform_glfw ALIAS avbase_platform_glfw)
    target_include_directories(avbase_platform_glfw PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
    # 注意 target 名是 `glfw` 而不是 `glfw3`：GLFW 官方的 CMake 包导出的 imported
    # target 叫 glfw，与它产出物的文件名 libglfw3.a 不一致。写成 glfw3 时 CMake 会
    # 把它降级为裸 `-lglfw3`，既不传 GLFW 的 include 目录也不传 -L，表现为编译期
    # 'GLFW/glfw3.h' file not found。
    target_link_libraries(avbase_platform_glfw
        PUBLIC avbase::media avbase::gl glfw)
    target_link_libraries(avbase_platform_glfw
        PRIVATE avbase::platform_hwimport)
    avbase_configure_target(avbase_platform_glfw)
  else()
    message(WARNING
        "AVBASE_ENABLE_GLFW=ON but glfw3 not found; GLFW backend skipped")
  endif()
endif()

# EGL backend: OPTIONAL. Same adaptation role as the others; the host owns the
# EGLDisplay/EGLSurface/EGLContext. Enable with -DAVBASE_ENABLE_EGL=ON; skipped
# if EGL is not installed.
option(AVBASE_ENABLE_EGL "Build the EGL GL backend" OFF)
if(AVBASE_ENABLE_EGL)
  find_package(EGL QUIET)
  if(EGL_FOUND)
    file(GLOB AVBASE_EGL_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/egl/*.cc")
    add_library(avbase_platform_egl ${AVBASE_EGL_SOURCES})
    add_library(avbase::platform_egl ALIAS avbase_platform_egl)
    target_include_directories(avbase_platform_egl PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
    target_link_libraries(avbase_platform_egl
        PUBLIC avbase::media avbase::gl EGL::EGL)
    target_link_libraries(avbase_platform_egl
        PRIVATE avbase::platform_hwimport)
    avbase_configure_target(avbase_platform_egl)
  else()
    message(WARNING
        "AVBASE_ENABLE_EGL=ON but EGL not found; EGL backend skipped")
  endif()
endif()

if(AVBASE_ENABLE_LINUX_NATIVE)
  message(FATAL_ERROR "AVBASE_ENABLE_LINUX_NATIVE is scheduled for milestone M12")
endif()


# The FFmpeg adapter target used to live here. It moved to media/ffmpeg/:
# FFmpeg is not a platform backend but a portable framework, and the
# directory a target lives in has to equal the target (docs/06 §7.6).
# See media/CMakeLists.txt and decision D11 in README.md.
