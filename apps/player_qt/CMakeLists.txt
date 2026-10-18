# The Qt Quick player / editor skeleton.
#
# WHY THIS DOES NOT CALL avbase_configure_target(): that helper adds
# -fno-exceptions -fno-rtti, which is the right contract for the core libraries
# and the wrong one here. Qt's meta-object system needs RTTI -- qobject_cast,
# QML's runtime type conversion and moc's generated casts all use it -- so
# compiling this target with -fno-rtti fails inside moc output rather than at
# anything the author wrote. A UI app is a consumer of the SDK, so it gets the
# consumer's compiler contract, not the library's.
#
# The reverse edge is fine and expected: this target links avbase::avbase, which
# was itself built without exceptions or RTTI.

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Quick QuickControls2 OpenGL)

# QOpenGLFramebufferObject lives in Qt's OpenGL module (it moved out of Gui in
# Qt 6), so the GPU item needs it: it is the object Qt asks us to allocate and
# the buffer our renderer draws into.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

qt_add_executable(avbase_player_qt
    gl_video_item.cc
    gl_video_item.h
    gl_video_renderer.cc
    gl_video_renderer.h
    main.cc
    player_bridge.cc
    player_bridge.h
    qt_video_sink.cc
    qt_video_sink.h
    timeline_model.cc
    timeline_model.h
    video_item.cc
    video_item.h
)

# ABI: the layout-affecting macros the core libraries were built with. This
# target cannot take the library's -fno-rtti, but it MUST see the same header
# layouts -- SEQUENCE_CHECKER is a member or nothing depending on
# AVBASE_ENABLE_DCHECK, and a scoped_refptr whose AddRef() writes to the wrong
# offset frees frames out from under the compositor. See the comment on
# avbase_configure_abi_definitions() in cmake/AvbaseCompilerFlags.cmake.
avbase_configure_abi_definitions(avbase_player_qt)

target_include_directories(avbase_player_qt PRIVATE "${CMAKE_SOURCE_DIR}")

# QML is loaded from the source tree rather than from a compiled-in resource:
# the skeleton is meant to be edited and re-run, and a qrc round trip on every
# tweak is friction with no payoff before there is a product to ship. The path
# is a compile-time constant so there is no runtime lookup to get wrong.
target_compile_definitions(avbase_player_qt PRIVATE
    AVBASE_QT_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")

target_link_libraries(avbase_player_qt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::OpenGL
    avbase::avbase
)

# The GPU surface needs the Qt window-system adapter from platform/qt. It is a
# hard requirement rather than an optional extra: the software item is slow
# enough that shipping the app without the GPU path would misrepresent what the
# skeleton is for, and a silently-degraded build is the kind of thing nobody
# notices until they profile it. Say which flag is missing instead.
if(NOT TARGET avbase::platform_qt)
  message(FATAL_ERROR
      "AVBASE_BUILD_QT_PLAYER=ON requires -DAVBASE_ENABLE_QT=ON "
      "(the GPU video surface links avbase::platform_qt)")
endif()
target_link_libraries(avbase_player_qt PRIVATE avbase::platform_qt)

# Real audio output. Sdl2AudioSink pulls Render() from SDL's audio thread and
# lazily runs SDL_INIT_AUDIO itself, so this needs no SDL window and no init in
# main() -- Qt owns the window, SDL only owns the sound card.
#
# Without it the app still builds and plays, silently: avbase falls back to its
# null sink, which is the same thing play_glfw does.
if(AVBASE_ENABLE_SDL2)
  target_compile_definitions(avbase_player_qt PRIVATE
      AVBASE_QT_HAVE_SDL2_AUDIO=1)
endif()
