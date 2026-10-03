# media/ — media framework. media/base holds interfaces and value types,
# media/filters holds implementations. Neither links FFmpeg (that is
# avbase_platform_ffmpeg); see docs/02 §8 and decision D11.
#
# Source listing (docs/06 §7.6): base/, renderers/ and filters/legacy/ are
# globbed because in each of them directory membership equals target membership
# -- every .cc there belongs to avbase_media, and none is selected per platform
# or per build option, so a new file needs no edit here. filters/ stays a
# hand-written list for the opposite reason: it holds the core filters next to
# the ffmpeg_*.cc files that only avbase_platform_ffmpeg may compile (G2), and
# a glob there would sweep libav* into the core target. Invariant C25 fails
# the gate when a .cc lands in no target at all, so "forgot to list it" cannot
# stay silent.
file(GLOB AVBASE_MEDIA_BASE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/base/*.cc")
file(GLOB AVBASE_MEDIA_LEGACY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/filters/legacy/*.cc")
file(GLOB AVBASE_MEDIA_RENDERER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/renderers/*.cc")

add_library(avbase_media
    # The Pipeline interfaces in this glob were promoted from DRAFT in the tenth
    # round. Each is the key function for its class's vtable, which is why their
    # constructors and destructors are out-of-line (see media/base/demuxer.cc).
    ${AVBASE_MEDIA_BASE_SOURCES}
    filters/audio_frame_queue.cc
    filters/audio_renderer_algorithm.cc
    filters/audio_renderer_impl.cc
    filters/audio_renderer_ring.cc
    filters/decoder_selector.cc
    filters/live_data_source.cc
    filters/live_edge_policy.cc
    filters/decoder_stream.cc
    filters/null_audio_sink.cc
    filters/null_video_sink.cc
    filters/pipeline_impl.cc
    filters/pipeline_track_select.cc
    filters/pipeline_impl_host.cc
    filters/retry_data_source.cc
    filters/renderer_impl.cc
    filters/renderer_impl_controls.cc
    filters/video_renderer_impl.cc
    filters/wsola_internals.cc
    ${AVBASE_MEDIA_RENDERER_SOURCES}
    # LGPL-2.1 quarantine: line-by-line ports of ff_ffplay.c algorithms.
    # See media/filters/legacy/README.md and root LICENSE section 1.
    ${AVBASE_MEDIA_LEGACY_SOURCES})

# FFmpeg-backed demuxer. Compiled into avbase_platform_ffmpeg, never into
# avbase_media, so that the core target carries no FFmpeg dependency (G2).
add_library(avbase::media ALIAS avbase_media)

target_include_directories(avbase_media PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/avbase>)
target_link_libraries(avbase_media PUBLIC avbase::base)
target_compile_definitions(avbase_media PRIVATE AVBASE_MEDIA_IMPLEMENTATION=1)
avbase_configure_target(avbase_media)
