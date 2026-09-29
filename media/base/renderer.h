// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/renderer.h` (BSD-3-Clause), minus the
// browser-only surface: SetLatencyHint stays (live playback needs it) but
// RequestOverlayInfo, the CDM key-system callbacks and the
// MediaFoundation/MediaCodec renderer-type proliferation do not.
//
// STATUS: DRAFT — NOT YET IN THE BUILD (milestone M7, docs/08 §2).
// Interface frozen so M7 and M8 can be written in parallel. Excluded from
// every CMake target on purpose; see media/base/pipeline_status.h for the
// DRAFT convention this project uses.
//
// Gaps to close before this file joins the build:
//   1. The only implementation is media/filters/renderer_impl.cc (M7), which
//      composes VideoRendererImpl + AudioRendererImpl + TextRenderer +
//      AvSyncController (the latter two already exist, under
//      media/filters/legacy/).
//   2. SetCdm takes a CdmContext* that is forward-declared in
//      media/base/video_decoder.h and has no definition: decision D8 does not
//      implement DRM. Keep it forward-declared; do not create cdm_context.h
//      until D8 is revisited, or the empty interface becomes an API promise.
//   3. GetRendererType() has one real answer today (kRendererImpl). The other
//      enumerators exist so that a business-specific renderer can be selected
//      through RendererFactory without widening this interface.
//   4. OnTracksChanged's |enabled_track| is a raw pointer whose lifetime is
//      the MediaResource's. That matches Demuxer::GetStream() (frozen at M4)
//      but needs a DCHECK-and-document pass once SelectTrack() (M8) can null
//      it mid-playback.
//
// .cc owed by this header -- media/base/renderer.cc:
//   RendererTypeToString(RendererType)
//   Renderer::Renderer()      -- out-of-line `= default`, matching the
//   Renderer::~Renderer()        convention in media/base/demuxer.cc and
//                                video_decoder.cc, which keeps the vtable and
//                                the key function out of every includer
//   Renderer::SetCdm(CdmContext*, OnceCallback<void(bool)>)
//       -- the only non-pure virtual. Its default implementation must run
//       cdm_attached_cb with false rather than drop it: D8 leaves DRM
//       unimplemented, and a caller waiting on that callback would otherwise
//       hang, which is the exact failure class Δ15 exists to prevent.

#ifndef IJKPP_MEDIA_BASE_RENDERER_H_
#define IJKPP_MEDIA_BASE_RENDERER_H_

#include <optional>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "media/base/demuxer_stream.h"
#include "media/base/media_resource.h"
#include "media/base/pipeline_status.h"
#include "media/base/renderer_client.h"
#include "media/media_export.h"

namespace ijkpp::media {

// DRM placeholder, not implemented (decision D8, docs/08 §4).
class CdmContext;

// Which Renderer implementation the pipeline should build.
enum class RendererType {
  kRendererImpl = 0,   // Default: FFmpeg decode + ijkpp's own sinks.
  kNullRenderer,       // Headless. platform/null, M10. CI and transcode-style
                       // decode-to-nowhere runs.
  kCastRenderer,       // Reserved; not planned in this milestone set.
};

IJKPP_MEDIA_EXPORT const char* RendererTypeToString(RendererType type);

// Turns demuxed data into audio and video output.
//
// WHAT IT REPLACES. In ijkplayer this role is played by the whole of
// ff_ffplay.c: a 5400-line translation unit whose VideoState god-struct holds
// ~200 fields that any of four threads may touch. Here it is one interface
// whose implementation (RendererImpl) owns three sub-renderers with at most 15
// private fields each, and whose frame-pacing decision lives in a pure static
// function that can be exhaustively unit tested
// (VideoFrameCompositor::DecideNextFrame, 55 tests today). See docs/01 §2
// for the twelve defects this shape exists to avoid.
//
// THREADING. Every method runs on the media sequence except GetMediaTime(),
// which is explicitly thread-safe: the SDK facade reads it from the caller's
// thread to answer Player::GetMediaTime() without a PostTask round trip. The
// implementation must therefore read the clock through AvSyncController's
// seqlock (Δ14) rather than through a mutex.
//
// ORDERING CONTRACT. Initialize() must complete before any other method;
// StartPlayingFrom() must complete before Play-affecting calls. Violating
// either is a DCHECK in debug builds and a no-op returning kInvalidState in
// release -- never undefined behaviour, because the caller here is the SDK
// facade and its users are not in a position to know the sequence rules.
class IJKPP_MEDIA_EXPORT Renderer {
 public:
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  virtual ~Renderer();

  // Prepares to render from |media_resource|. |init_cb| runs on the media
  // sequence and is never run inline, so a caller may safely destroy state in
  // it. kOk on success; any other PipelineStatus is convertible to a MediaError
  // with an actionable message (pipeline_status.h).
  virtual void Initialize(MediaResource* media_resource,
                          RendererClient* client,
                          base::scoped_refptr<base::SequencedTaskRunner>
                              media_task_runner,
                          PipelineStatusCallback init_cb) = 0;

  // Attaches a DRM context. Not implemented (D8): the default returns false
  // through |cdm_attached_cb| so that a caller learns the truth instead of
  // silently playing unprotected content.
  virtual void SetCdm(CdmContext* cdm_context,
                      base::OnceCallback<void(bool)> cdm_attached_cb);

  // Target latency for live streams. nullopt means "not live, buffer freely";
  // a value means "drop rather than fall further behind than this"
  // (config.net.live_max_latency, M9).
  virtual void SetLatencyHint(std::optional<base::TimeDelta> latency_hint) = 0;

  // When playback_rate != 1.0, whether to keep pitch (WSOLA time-stretch) or
  // let it rise/fall with speed. config.audio.preserves_pitch.
  virtual void SetPreservesPitch(bool preserves_pitch) = 0;

  // Whether to keep pushing silence through the audio sink while muted. True
  // keeps the audio clock authoritative, which is what makes SetMuted() not
  // disturb A/V sync; false saves a little CPU at the cost of falling back to
  // the external clock.
  virtual void SetRenderMutedAudio(bool render_muted_audio) = 0;

  // ---- Valid only after Initialize() -------------------------------------

  // Drops every buffered packet and frame, then runs |flush_cb|. After the
  // callback, no data from a previous serial remains and no callback from the
  // previous generation can still fire -- that guarantee is what makes
  // Player::SeekTo() safe to call repeatedly (docs/04 §4.1).
  virtual void Flush(base::OnceClosure flush_cb) = 0;

  // Begins rendering at |time|, clamped to [0, duration]. Idempotent for the
  // same position; a second call before the first has produced a frame
  // supersedes it (ffplay's serial mechanism, kept verbatim).
  virtual void StartPlayingFrom(base::TimeDelta time) = 0;

  // [config.min_playback_rate, config.max_playback_rate], i.e. 0.25..4.0.
  // Out-of-range values are clamped, not rejected: a UI slider should not be
  // able to put the player into an error state.
  virtual void SetPlaybackRate(double playback_rate) = 0;

  // 0.0 .. 1.0 (Δ6: ijkplayer's 0..100 is converted in OptionRegistry).
  virtual void SetVolume(float volume) = 0;

  // Thread-safe. The authoritative media time, from whichever clock
  // AvSyncController resolved as master. Returns kNoTimestamp before the first
  // frame is rendered.
  virtual base::TimeDelta GetMediaTime() = 0;

  // Track selection changed. |enabled_track| is nullptr to disable that track
  // type entirely (config.video.disabled / config.audio.disabled).
  virtual void OnTracksChanged(DemuxerStreamType track_type,
                               DemuxerStream* enabled_track,
                               base::OnceClosure change_completed_cb) = 0;

  virtual RendererType GetRendererType() = 0;

 protected:
  Renderer();
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_RENDERER_H_
