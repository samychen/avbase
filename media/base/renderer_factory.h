// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/renderer_factory.h` (BSD-3-Clause) in role,
// not in signature: Chromium's version is a selector over mojo/renderer types
// for the browser process, whereas ijkpp's is the seam through which the SDK
// facade injects platform backends. That difference is deliberate and is the
// mechanism behind the "zero configuration works" requirement in docs/10 §2.
//
// STATUS: DRAFT — NOT YET IN THE BUILD (milestone M8, docs/08 §2).
// Interface frozen so M7 and M8 can be written in parallel. Excluded from
// every CMake target on purpose; see media/base/pipeline_status.h for the
// DRAFT convention this project uses.
//
// Gaps to close before this file joins the build:
//   1. DefaultRendererFactory (media/renderers/, M7) is the production
//      implementation; platform/null (M10), platform/sdl2 (M11) and
//      platform/linux (M12) each supply sinks to it rather than implementing
//      this interface themselves.
//   2. player/public/deps.h (frozen at M8, 79 lines) is where a business
//      injects its own factory. The wiring from Deps to
//      Pipeline::Start(renderer_factory) does not exist yet.
//   3. CreateVideoRendererSink()'s NativeDisplay argument is nullable, which is
//      how headless playback (examples/headless) and config.render.
//      disable_video_output are expressed. platform/null must accept
//      nullptr and still honour VideoRendererSinkContract; that is the test's
//      SetOutputTargetNullEntersDiscardMode case (docs/07 §4).
//   4. THIS INTERFACE OVERLAPS THREE EXISTING INJECTION PATHS, and M8 must
//      collapse them into one story instead of letting them drift:
//        a. media::VideoRendererSinkFactory / AudioRendererSinkFactory
//           (media/base/*_renderer_sink.h) -- already exist, and are already
//           what player::Deps holds.
//        b. Player::SetVideoSurface(scoped_refptr<NativeDisplay>) --
//           retargets a live sink, so it is NOT a factory path.
//        c. Create*RendererSink() below.
//      The intended relation: DefaultRendererFactory (M7) is *constructed from*
//      (a) and answers (c) with what it was given, so (c) is an internal seam
//      and (a) is the public one. That belongs in DefaultRendererFactory's
//      header, and it needs the shared_ptr/scoped_refptr conflict in
//      player/public/deps.h settled first -- see docs/PROGRESS.md, ninth round,
//      finding F2.
//
// .cc owed by this header: none. Every member is pure virtual or deleted, so it
// needs no translation unit of its own; the sinks it returns are what owe .cc
// files (platform/null at M10, platform/sdl2 at M11).

#ifndef IJKPP_MEDIA_BASE_RENDERER_FACTORY_H_
#define IJKPP_MEDIA_BASE_RENDERER_FACTORY_H_

#include <memory>

#include "base/memory/scoped_refptr.h"
#include "media/base/renderer.h"
#include "media/media_export.h"

namespace ijkpp::media {

// Forward-declared rather than included, matching the convention already set by
// media/base/video_decoder_factory.h (which forward-declares VideoDecoder and
// VideoDecoderConfig) and player/public/deps.h. Every one of these is used here
// only through a pointer or a smart pointer, so none needs to be complete.
// Not including them also keeps this header's transitive closure small,
// which matters because every platform backend includes it.
class AudioDecoderFactory;
class AudioRendererSink;
class NativeDisplay;
class VideoDecoderFactory;
class VideoRendererSink;

// Builds the renderer and the two output endpoints for one playback.
//
// WHY THIS IS THE PLATFORM SEAM. platform/ may depend on media/ and base/ but
// never the other way round (docs/02 §2.1, invariant C22). So the media layer
// cannot name Sdl2VideoSink or AlsaAudioRendererSink; it can only ask a factory
// for "a video sink for this display". player/ chooses the factory -- by
// auto-detecting the backend per docs/09 §2.1, or by taking one from
// player::Deps -- and the rest of the pipeline never learns which platform it
// is running on. That is also what makes the NullSink CI configuration and a
// business-specific renderer the same code path rather than two.
//
// Threading: called on the media sequence. Implementations must not block --
// backend probing (dlopen, X11/Wayland connection) belongs in the
// platform's Detect() step, which runs once at Player construction, not here.
class IJKPP_MEDIA_EXPORT RendererFactory {
 public:
  RendererFactory(const RendererFactory&) = delete;
  RendererFactory& operator=(const RendererFactory&) = delete;

  // Returns nullptr when this factory cannot serve |type|, which lets a
  // chained factory fall through to the next one -- the same fallback shape
  // DecoderSelector already uses for decoders (Δ12: report kDecoderFallback
  // and continue rather than fail).
  virtual std::unique_ptr<Renderer> CreateRenderer(
      RendererType type,
      base::scoped_refptr<base::SequencedTaskRunner> media_task_runner) = 0;

  // |display| is nullptr for headless playback or when video output is
  // disabled; the sink must then discard frames while still driving the
  // render cadence, so that the video clock keeps advancing.
  virtual std::unique_ptr<VideoRendererSink> CreateVideoRendererSink(
      base::scoped_refptr<NativeDisplay> display) = 0;

  // Audio has no display argument: the device is chosen by
  // config.audio.output_device_id and the backend by config.audio.backend
  // (docs/09 §5.1's priority order PipeWire > Pulse > ALSA > SDL2 > Null).
  virtual base::scoped_refptr<AudioRendererSink> CreateAudioRendererSink() = 0;

  // Decoder factories, so that a platform can offer hardware decoders without
  // media/filters knowing about them. DefaultDecoderFactory (M7) supplies the
  // FFmpeg ones; M14 adds VAAPI, M16 MediaCodec, M17 VideoToolbox.
  virtual VideoDecoderFactory* GetVideoDecoderFactory() = 0;
  virtual AudioDecoderFactory* GetAudioDecoderFactory() = 0;

  // Short stable name for logs and DumpDiagnostics(), e.g. "sdl2", "linux-gl",
  // "null". Must not be null.
  virtual const char* name() const = 0;

 protected:
  RendererFactory() = default;
  virtual ~RendererFactory() = default;
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_RENDERER_FACTORY_H_
