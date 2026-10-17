# player/public/c/ -- the C ABI compatibility layer (docs/08 M15).
#
# A SEPARATE library, not a source file added to avbase_player. The reason is
# the consumer: a C or JNI host links this target and must not acquire the C++
# headers or the C++ ABI as a dependency. Making it part of avbase_player would
# force exactly that, and the C ABI would stop being optional in practice.
#
# Enabled by -DAVBASE_ENABLE_CAPI=ON; off by default because a project that only
# uses the C++ API should not pay for it.
# One .cc per surface (player / display / events): the whole C ABI in one file
# runs past the C1 500-line limit, and the split follows the three responsibilities
# rather than being an arbitrary chop. avbase_c_internal.h holds what they share.
add_library(avbase_cabi STATIC
    avbase_c_player.cc
    avbase_c_display.cc
    avbase_c_events.cc)

target_include_directories(avbase_cabi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/avbase>)

target_compile_definitions(avbase_cabi PRIVATE AVBASE_C_IMPLEMENTATION=1)
# PUBLIC: the header is the interface, so a consumer of this target needs the
# include path to find avbase_c.h itself.
target_link_libraries(avbase_cabi PUBLIC avbase::player)
avbase_configure_target(avbase_cabi)

# Installed as its own header so a C host can be handed ONLY this file plus a
# platform's window handle -- not the whole C++ public surface.
install(FILES avbase_c.h avbase_c_export.h
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/avbase/player/public/c")