# examples/ — the SDK usability bar (docs/10 §6): main.cc 行数是 API 易用性
# 度量。headless 只需要 FFmpeg；play_sdl2 还需要 SDL2 后端。
#
# 源文件保持逐个列出（docs/06 §7.6）：两个可执行文件同在一个目录、却在两个
# 不同的开关下，目录成员 ≠ 目标成员，glob 表达不了。

if(AVBASE_ENABLE_FFMPEG)
  add_executable(headless headless.cc)
  target_link_libraries(headless PRIVATE avbase::avbase)
  avbase_configure_target(headless)

  # Phase 3: the zero-copy frame-acquisition example. Needs the FFmpeg layer
  # for decode, and shows NativeHandle frames when a hardware factory is
  # enabled, but builds (and prints storage=cpu) without any of them.
  add_executable(pull_frames pull_frames.cc)
  target_link_libraries(pull_frames PRIVATE avbase::avbase)
  avbase_configure_target(pull_frames)
endif()

if(AVBASE_ENABLE_FFMPEG AND AVBASE_ENABLE_SDL2)
  add_executable(play_sdl2 play_sdl2.cc)
  target_link_libraries(play_sdl2 PRIVATE avbase::avbase avbase::platform_sdl2)
  avbase_configure_target(play_sdl2)
endif()

# GLFW backend example. Gated the same way as the backend: built only when
# -DAVBASE_ENABLE_GLFW=ON AND glfw3 was found (the TARGET guard avoids a hard
# configure error when the option is on but the library is missing -- the
# backend's WARNING already explained that). avbase::platform_glfw propagates
# the glfw3 include path and library.
if(AVBASE_ENABLE_GLFW AND TARGET avbase::platform_glfw)
  add_executable(play_glfw play_glfw.cc)
  target_link_libraries(play_glfw PRIVATE avbase::avbase avbase::platform_glfw)
  avbase_configure_target(play_glfw)
endif()

# EGL backend example. Same gating shape as GLFW. Not buildable on macOS (no
# EGL dev libraries); see play_egl.cc's header for the Linux-only caveats.
if(AVBASE_ENABLE_EGL AND TARGET avbase::platform_egl)
  add_executable(play_egl play_egl.cc)
  target_link_libraries(play_egl PRIVATE avbase::avbase avbase::platform_egl)
  avbase_configure_target(play_egl)
endif()
