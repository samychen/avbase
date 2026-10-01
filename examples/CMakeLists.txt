# examples/ — the SDK usability bar (docs/10 §6): main.cc 行数是 API 易用性
# 度量。headless 只需要 FFmpeg；play_sdl2 还需要 SDL2 后端。

if(IJKPP_ENABLE_FFMPEG)
  add_executable(headless headless.cc)
  target_link_libraries(headless PRIVATE ijkpp::ijkpp)
  ijkpp_configure_target(headless)
endif()

if(IJKPP_ENABLE_FFMPEG AND IJKPP_ENABLE_SDL2)
  add_executable(play_sdl2 play_sdl2.cc)
  target_link_libraries(play_sdl2 PRIVATE ijkpp::ijkpp ijkpp::platform_sdl2)
  ijkpp_configure_target(play_sdl2)
endif()
