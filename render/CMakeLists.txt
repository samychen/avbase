# render/ — the graphics-API layer: how a decoded frame becomes pixels through
# a GPU API. Two axes meet in the tree and they are NOT the same axis:
#
#   graphics API   render/gl/  (later render/metal/, render/d3d11/ ...)
#   window system  platform/sdl2, platform/glfw, platform/egl
#
# This used to be platform/gl/, a sibling of the window-system backends. That
# flattens both axes into one directory level, and a flat layout does not
# compose: a second graphics API would demand platform/{gl,metal}x{sdl2,glfw,egl}
# = six directories, including combinations that cannot exist. See docs/02 §6.
#
# It also failed that section's own test for belonging in platform/: platform/
# holds code the HOST SELECTS, and nothing selects this layer. Every backend
# links it unconditionally, it carries no window-system SDK at all (no GL
# header anywhere -- entry points arrive through NativeGLContext::
# GetProcAddress), and it behaves identically on every host. Those are the same
# three reasons FFmpeg left platform/ for media/ffmpeg/ (decision D11).
#
# It cannot move into media/ either: invariant C5 forbids third-party platform
# headers under media/. So it is a layer of its own, depending on media only.
#
# Direction: render -> media, and nothing in render/ may include platform/. The
# window system reaches this layer only through the NativeGLContext interface
# each backend implements (render/gl/native_gl_context.h).
file(GLOB AVBASE_RENDER_GL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/gl/*.cc")
add_library(avbase_render_gl ${AVBASE_RENDER_GL_SOURCES})
add_library(avbase::render_gl ALIAS avbase_render_gl)
target_include_directories(avbase_render_gl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
target_link_libraries(avbase_render_gl PUBLIC avbase::media)
avbase_configure_target(avbase_render_gl)
