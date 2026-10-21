# player/ — the SDK facade. Only player/public/*.h is installed.
#
# Source listing (docs/06 §7.6): one directory, one target, nothing selected per
# platform or per build option -- so the list is a glob and a new file needs no
# edit here. Invariant C25 catches a .cc that lands in no target at all.
file(GLOB AVBASE_PLAYER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
add_library(avbase_player ${AVBASE_PLAYER_SOURCES})
add_library(avbase::player ALIAS avbase_player)
target_include_directories(avbase_player PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/avbase>)
target_link_libraries(avbase_player PUBLIC avbase::media)
# The facade assembles the default decoder-factory list (player_impl.cc),
# including the FFmpeg-hwaccel hardware factory whose specs live in
# platform/{videotoolbox,vaapi,d3d11} (header-only; the factory class itself
# is in avbase_ffmpeg).
if(AVBASE_ENABLE_FFMPEG)
  target_link_libraries(avbase_player PUBLIC avbase::ffmpeg)
endif()
target_compile_definitions(avbase_player PRIVATE AVBASE_PLAYER_IMPLEMENTATION=1)
avbase_configure_target(avbase_player)

add_library(avbase INTERFACE)
add_library(avbase::avbase ALIAS avbase)
target_link_libraries(avbase INTERFACE avbase::player)
if(AVBASE_ENABLE_FFMPEG)
  target_link_libraries(avbase INTERFACE avbase::ffmpeg)
endif()
if(AVBASE_ENABLE_SDL2)
  target_link_libraries(avbase INTERFACE avbase::platform_sdl2)
endif()

# player/qt/ — the Qt Quick facade. A subdirectory rather than a sibling of
# player/ because it is part of the SDK's host surface (apps/ is for consumers
# and nothing under base/ media/ player/ render/ platform/ may include it),
# and its own target because the whole directory is what one option selects
# (docs/06 §7.6: one directory, one target).
if(AVBASE_BUILD_QT_FACADE)
  add_subdirectory(qt)
endif()
