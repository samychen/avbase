# player/ — the SDK facade. Only player/public/*.h is installed.
add_library(ijkpp_player
    deps.cc
    event_hub.cc
    global.cc
    option_registry.cc
    playback_stats.cc
    player.cc
    player_config.cc
    player_event.cc
    player_impl.cc
    player_impl_events.cc
    state_machine.cc)
add_library(ijkpp::player ALIAS ijkpp_player)
target_include_directories(ijkpp_player PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ijkpp>)
target_link_libraries(ijkpp_player PUBLIC ijkpp::media)
target_compile_definitions(ijkpp_player PRIVATE IJKPP_PLAYER_IMPLEMENTATION=1)
ijkpp_configure_target(ijkpp_player)

add_library(ijkpp INTERFACE)
add_library(ijkpp::ijkpp ALIAS ijkpp)
target_link_libraries(ijkpp INTERFACE ijkpp::player)
if(IJKPP_ENABLE_FFMPEG)
  target_link_libraries(ijkpp INTERFACE ijkpp::platform_ffmpeg)
endif()
if(IJKPP_ENABLE_SDL2)
  target_link_libraries(ijkpp INTERFACE ijkpp::platform_sdl2)
endif()
