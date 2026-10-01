# player/ — the SDK facade. Only player/public/*.h is installed.
#
# Source listing (docs/06 §7.6): one directory, one target, nothing selected per
# platform or per build option -- so the list is a glob and a new file needs no
# edit here. Invariant C25 catches a .cc that lands in no target at all.
file(GLOB IJKPP_PLAYER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
add_library(ijkpp_player ${IJKPP_PLAYER_SOURCES})
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
