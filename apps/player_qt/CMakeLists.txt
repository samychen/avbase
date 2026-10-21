# The Qt Quick player / editor skeleton — the app half.
#
# Everything reusable (PlayerBridge, VideoItem, GlVideoItem, GlVideoRenderer,
# QtVideoSink, FrameMailbox, TimelineModel) lives in player/qt/ now and is
# linked as avbase::qtfacade. This directory holds what is genuinely the app:
# main.cc (window setup, CLI switches, QML loading) and the QML itself.
#
# WHY THIS DOES NOT CALL avbase_configure_target(): that helper adds
# -fno-exceptions -fno-rtti, which is the right contract for the core libraries
# and the wrong one here. Qt's meta-object system needs RTTI -- qobject_cast,
# QML's runtime type conversion and moc's generated casts all use it -- so
# compiling this target with -fno-rtti fails inside moc output rather than at
# anything the author wrote. A UI app is a consumer of the SDK, so it gets the
# consumer's compiler contract, not the library's.
#
# The reverse edge is fine and expected: this target links avbase::qtfacade,
# which relays to avbase::avbase, built without exceptions or RTTI.

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Quick QuickControls2 OpenGL)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

qt_add_executable(avbase_player_qt
    main.cc
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
    # QuickControls2 is a QML import, not a C++ dependency of the facade, so it
    # is linked here and not there: the library has no QML of its own.
    Qt6::QuickControls2
    Qt6::OpenGL
    avbase::qtfacade
)

if(NOT TARGET avbase::qtfacade)
  message(FATAL_ERROR
      "avbase_player_qt requires avbase::qtfacade "
      "(-DAVBASE_BUILD_QT_FACADE=ON, implied by AVBASE_BUILD_QT_PLAYER)")
endif()
