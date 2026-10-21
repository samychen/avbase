# player/qt/ — the reusable Qt Quick facade.
#
# WHY THIS LIVES IN player/ AND NOT IN apps/: everything here is a *hosting*
# component, not a demo. PlayerBridge, VideoItem/GlVideoItem, QtVideoSink and
# TimelineModel are the four things any Qt Quick host needs to show a frame and
# drive a clock, and until they lived in apps/player_qt/ the only way to use
# them was to copy the directory into your own app. An app is a consumer of the
# SDK (apps/CMakeLists.txt); a facade IS part of the SDK's host surface, and
# "the host has to copy it" is the same failure as "the host has to reimplement
# it", just slower to notice.
#
# WHY THIS DOES NOT CALL avbase_configure_target(): same reason as the app and
# the Qt unit test. That helper adds -fno-exceptions -fno-rtti, and Qt's
# meta-object system needs RTTI (qobject_cast, QML's runtime type conversion,
# moc's generated casts). The compile fails inside moc output rather than at
# anything written here. The layout-affecting macros must still match, hence
# avbase_configure_abi_definitions() below -- see the comment on it in
# cmake/AvbaseCompilerFlags.cmake.
#
# Source listing (docs/06 §7.6): one directory, one target. The directory as a
# whole is what AVBASE_BUILD_QT_FACADE selects, so the list is a glob and a new
# file needs no edit here.

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Quick OpenGL)

set(CMAKE_AUTOMOC ON)

file(GLOB AVBASE_QT_FACADE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
add_library(avbase_qtfacade ${AVBASE_QT_FACADE_SOURCES})
add_library(avbase::qtfacade ALIAS avbase_qtfacade)

# PUBLIC, not PRIVATE: a host that links this needs the same include root,
# because its own code will #include "player/qt/player_bridge.h" to subclass or
# instantiate these types, and it needs the same Qt modules because the
# Q_PROPERTY types in these headers (QSize, QVariantList) are Qt's.
target_include_directories(avbase_qtfacade PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>)

avbase_configure_abi_definitions(avbase_qtfacade)

target_link_libraries(avbase_qtfacade PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::OpenGL
    avbase::avbase)

# media/gl (the YUV layer) and render/gl (the GL API/program/quad the layer is
# drawn with) are both reachable through avbase::avbase, but named explicitly
# because gl_video_renderer.cc includes their headers directly and a facade that
# compiles "by accident of transitive include dirs" breaks the first time the
# graph is tidied.
target_link_libraries(avbase_qtfacade PUBLIC avbase::gl)

# The GPU surface needs Qt's window-system adapter, whose MakeCurrent/Swap are
# deliberately inert (see platform/qt/qt_gl_context.h: Qt owns the swap). Hard
# requirement rather than optional: without it the facade cannot draw at all,
# and a facade that silently loses its GPU path is worse than one that fails
# to configure. Say which flag is missing instead.
#
# Checked as an OPTION rather than as `if(NOT TARGET avbase::platform_qt)`,
# which is what the app used to do and which cannot work here: player/ is
# added before platform/ (CMakeLists.txt), so the ALIAS does not exist yet
# and that test is false on every correct configuration. The two checks are
# complementary on purpose -- this one names the missing flag at configure
# time, while the target_link_libraries below is what fails (at generate time,
# when every directory has been read) if Qt was requested but not found.
if(NOT AVBASE_ENABLE_QT)
  message(FATAL_ERROR
      "AVBASE_BUILD_QT_FACADE=ON requires -DAVBASE_ENABLE_QT=ON "
      "(the GPU video surface links avbase::platform_qt)")
endif()
target_link_libraries(avbase_qtfacade PUBLIC avbase::platform_qt)

# Real audio output. Sdl2AudioSink pulls Render() from SDL's audio thread and
# lazily runs SDL_INIT_AUDIO itself, so it needs no SDL window and no init in
# the host's main() -- Qt owns the window, SDL only owns the sound card.
# Without it playback still works, silently, through avbase's null sink; the
# macro exists so the host can tell the difference, not to make SDL required.
if(AVBASE_ENABLE_SDL2)
  target_compile_definitions(avbase_qtfacade PUBLIC
      AVBASE_QT_HAVE_SDL2_AUDIO=1)
endif()
