// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_SDL2_SDL2_OUTPUT_TARGET_H_
#define AVBASE_PLATFORM_SDL2_SDL2_OUTPUT_TARGET_H_

#include "media/gl/gl_output_target.h"
#include "media/media_export.h"
#include "render/gl/overlay.h"

namespace avbase::media {

// The SDL2 half of video output: everything that used to make Sdl2VideoSink a
// SECOND sink rather than a backend.
//
// It is one target that can answer two different ways, and which one applies is
// a property of the surface the host handed over:
//
//   * with an SDL_GLContext on the surface, it binds a GlPresenter through it
//     and the GL path draws (same presenter every other GL backend gets);
//   * without one, it draws through SDL_Renderer / SDL_UpdateYUVTexture, which
//     needs the plane bytes in CPU memory and is why this target is the one
//     that reports has_gl_route() == false.
//
// The render loop, the cadence, the stats and the scene driver are not here --
// they are GlVideoSink's, and they are the same for every backend. What is
// here is the part only SDL can answer.
//
// THREADING (see surface.h): the host created the window/renderer on its main
// thread; every SDL object below is then touched only from the sink's render
// thread, and ReleaseOutput() runs once that thread is joined.
class AVBASE_MEDIA_EXPORT Sdl2OutputTarget final : public GlOutputTarget {
 public:
  Sdl2OutputTarget() = default;
  Sdl2OutputTarget(const Sdl2OutputTarget&) = delete;
  Sdl2OutputTarget& operator=(const Sdl2OutputTarget&) = delete;
  ~Sdl2OutputTarget() override = default;

  // GlOutputTarget:
  void SetSurface(void* raw) override;
  bool has_gl_route() const override;
  GlSurfaceBinding BindSurface() override;
  bool PresentWithoutGl(const VideoFrame& frame,
                        PresentTiming* timing) override;
  void ReleaseOutput() override;

 private:
  void PresentOverlay(void* renderer);  // SDL_Renderer*.
  // The display's opaque surface pointer, kept untyped here for the same
  // reason the SDL objects below are: only the .cc names Sdl2Surface.
  void* raw_{nullptr};
  // Borrowed from the display's Sdl2Surface; may be null.
  gl::TextOverlaySlot* overlay_slot_{nullptr};

  // SDL types kept as void* in the header: platform/sdl2 may name SDL, but
  // every file that includes this one would then need the SDL include path,
  // and only the .cc does.
  void* renderer_{nullptr};
  void* gl_context_{nullptr};
  void* texture_{nullptr};
  int texture_width_{0};
  int texture_height_{0};
  // SDL_Renderer-path overlay state (mirrors the GL presenter's).
  void* overlay_texture_{nullptr};
  int overlay_width_{0};
  int overlay_height_{0};
  int last_overlay_version_{-1};
  // The last snapshot and which slot version it holds: the version is read
  // first so a frame that republished nothing copies nothing -- a subtitle
  // stays up for hundreds of frames and the bitmap is the expensive half.
  gl::TextOverlay overlay_;
  int snapshot_version_{-1};
};

}  // namespace avbase::media

#endif  // AVBASE_PLATFORM_SDL2_SDL2_OUTPUT_TARGET_H_
