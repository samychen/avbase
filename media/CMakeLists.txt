# media/ — media framework. media/base holds interfaces and value types,
# media/filters holds the portable implementations, media/ffmpeg holds
# everything that touches libav*, and media/transcode holds the offline
# transcode product line. See docs/02 §8 and decision D11.
#
# Source listing (docs/06 §7.6): base/, filters/, renderers/ and
# filters/legacy/ are all globbed, because in each of them directory
# membership equals target membership -- every .cc there belongs to
# avbase_media, and none is selected per platform or per build option, so a
# new file needs no edit here.
#
# filters/ used to be the exception: it held the ffmpeg_*.cc files next to the
# core filters, and because those belong to avbase_ffmpeg a glob would have
# swept libav* into the core target (G2), so it stayed a hand-written list.
# Both directories are single-target now that all the FFmpeg code has one home.
# Invariant C25 fails the gate when a .cc lands in no target at all, so
# "forgot to list it" cannot stay silent.
file(GLOB AVBASE_MEDIA_BASE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/base/*.cc")
file(GLOB AVBASE_MEDIA_FILTER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/filters/*.cc")
file(GLOB AVBASE_MEDIA_LEGACY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/filters/legacy/*.cc")
file(GLOB AVBASE_MEDIA_RENDERER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/renderers/*.cc")

add_library(avbase_media
    # The Pipeline interfaces in this glob were promoted from DRAFT in the tenth
    # round. Each is the key function for its class's vtable, which is why their
    # constructors and destructors are out-of-line (see media/base/demuxer.cc).
    ${AVBASE_MEDIA_BASE_SOURCES}
    ${AVBASE_MEDIA_FILTER_SOURCES}
    ${AVBASE_MEDIA_RENDERER_SOURCES}
    # LGPL-2.1 quarantine: line-by-line ports of ff_ffplay.c algorithms.
    # See media/filters/legacy/README.md and root LICENSE section 1.
    ${AVBASE_MEDIA_LEGACY_SOURCES})
add_library(avbase::media ALIAS avbase_media)

target_include_directories(avbase_media PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/avbase>)
target_link_libraries(avbase_media PUBLIC avbase::base)
target_compile_definitions(avbase_media PRIVATE AVBASE_MEDIA_IMPLEMENTATION=1)
avbase_configure_target(avbase_media)

# FFmpeg adapter layer. This is the ONLY target in the project that links the
# FFmpeg:: libraries; avbase_media and avbase_base never see a libav* header
# (design goal G2, invariants C4/C8, docs/02 §8). avbase_transcode, below,
# builds on this target and links no vendor library of its own.
#
# It sits under media/ and not platform/ because FFmpeg is not a platform. It
# is a portable framework that behaves identically on every host, whereas the
# platform layer (sdl2, hwaccel) holds code that is CHOSEN by the host. What
# earns this a separate target is the dependency; what earns it a separate
# directory is that a directory must equal a target for the globs above to be
# safe (docs/06 §7.6).
if(AVBASE_ENABLE_FFMPEG)
  # ffmpeg/ qualifies for a glob: every .cc in it belongs to this one target and
  # none is selected per platform (av_includes.h absorbs version differences).
  file(GLOB AVBASE_FFMPEG_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/*.cc")
  add_library(avbase_ffmpeg ${AVBASE_FFMPEG_SOURCES})
  add_library(avbase::ffmpeg ALIAS avbase_ffmpeg)

  target_include_directories(avbase_ffmpeg PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(avbase_ffmpeg
      PUBLIC  avbase::media
      PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil
              FFmpeg::swscale FFmpeg::swresample FFmpeg::avfilter)
  target_compile_definitions(avbase_ffmpeg PRIVATE AVBASE_ENABLE_FFMPEG=1)
  # Optional SIMD fast paths for video_convert.cc; when absent the module
  # compiles to a sws_scale-only fallback and nothing else changes.
  find_package(Libyuv QUIET)
  if(Libyuv_FOUND)
    target_link_libraries(avbase_ffmpeg PRIVATE libyuv::libyuv)
    target_compile_definitions(avbase_ffmpeg PUBLIC AVBASE_HAVE_LIBYUV=1)
  endif()
  avbase_configure_target(avbase_ffmpeg)
  # One precompiled header for the whole vendor layer: av_includes.h is the
  # single extern "C" wrap point, so every TU here needs it anyway.
  target_precompile_headers(avbase_ffmpeg PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/av_includes.h")

  # Offline transcode: the Phase 5 product line (encoders, mux writer, the
  # Transcode/Concat jobs, remux). It is FFmpeg code, so it is gated by the same
  # option and sits on top of avbase_ffmpeg -- but it is a separate TARGET
  # because nothing in the playback path uses it: the ten .cc here had zero
  # production consumers outside media/ffmpeg when this split was made (the one
  # caller that looked like one -- player's TakeSnapshot -- goes through
  # media/ffmpeg/ffmpeg_image_snapshot instead). Keeping it in avbase_ffmpeg
  # meant every playback build carried the encoder and muxer code; as its own
  # target a transcode consumer asks for avbase::transcode explicitly, and the
  # playback SDK stops paying for it. It is deliberately NOT part of the
  # avbase::avbase umbrella for the same reason.
  file(GLOB AVBASE_TRANSCODE_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/transcode/*.cc")
  add_library(avbase_transcode ${AVBASE_TRANSCODE_SOURCES})
  add_library(avbase::transcode ALIAS avbase_transcode)
  target_include_directories(avbase_transcode PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  # PUBLIC, not PRIVATE: transcode's public headers name media types, and its
  # .cc files reach into the av_includes.h / compat.h / video_convert.h glue.
  # The FFmpeg::* libraries themselves stay PRIVATE to avbase_ffmpeg, so this
  # target still links no vendor library of its own.
  target_link_libraries(avbase_transcode PUBLIC avbase::ffmpeg)
  # The transcode .cc files name libav* headers, exactly as the media/ffmpeg/
  # ones do -- include isolation was given up on purpose (D11 chose link
  # isolation plus directory isolation). What must not happen is a SECOND
  # target linking the FFmpeg:: libraries, which is why this asks for the
  # include directories rather than for FFmpeg::avcodec. Same shape as
  # media_ffmpeg_unittests and avbase-inspect, which add these for themselves:
  # avbase_ffmpeg links FFmpeg PRIVATE, so they are not inherited.
  target_include_directories(avbase_transcode SYSTEM PRIVATE
      ${FFmpeg_INCLUDE_DIRS})
  avbase_configure_target(avbase_transcode)
endif()
