// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/sdl2/sdl2_output_target.h"

#include <memory>
#include <utility>

#include "SDL.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "media/base/video_frame.h"
#include "platform/sdl2/sdl_gl_context.h"
#include "platform/sdl2/surface.h"

namespace avbase::media {
namespace {

// The target draws 8-bit planar YUV (what sws_scale produces for I420 output);
// anything else is reported as a submit failure rather than drawn wrong.
Uint32 SdlPixelFormat(VideoFormat format) {
  switch (format) {
  case VideoFormat::kI420:
    return SDL_PIXELFORMAT_IYUV;
  case VideoFormat::kYV12:
    return SDL_PIXELFORMAT_YV12;
  case VideoFormat::kNV12:
    return SDL_PIXELFORMAT_NV12;
  default:
    return SDL_PIXELFORMAT_UNKNOWN;
  }
}

}  // namespace

void Sdl2OutputTarget::SetSurface(void* raw) {
  raw_ = raw;
  if (!raw) {
    renderer_ = nullptr;
    gl_context_ = nullptr;
    overlay_slot_ = nullptr;
    return;
  }
  const auto* surface = static_cast<const Sdl2Surface*>(raw);
  renderer_ = surface->renderer;
  gl_context_ = surface->gl_context;
  overlay_slot_ = surface->overlay;
}

bool Sdl2OutputTarget::has_gl_route() const {
  // The only question that makes this backend different from the GL-only ones:
  // an SDL_GLContext on the surface is what puts presentation on the shader
  // path, and without it every frame goes through SDL_UpdateYUVTexture.
  return gl_context_ != nullptr;
}

GlSurfaceBinding Sdl2OutputTarget::BindSurface() {
  GlSurfaceBinding binding;
  if (!gl_context_ || !raw_) {
    return binding;
  }
  const auto* surface = static_cast<const Sdl2Surface*>(raw_);
  binding.context =
      std::make_unique<SdlGLContext>(surface->window, gl_context_);
  binding.overlay = overlay_slot_;
  binding.canvas_width = surface->canvas_width;
  binding.canvas_height = surface->canvas_height;
  return binding;
}

bool Sdl2OutputTarget::PresentWithoutGl(const VideoFrame& frame,
                                        PresentTiming* timing) {
  SDL_Renderer* renderer = static_cast<SDL_Renderer*>(renderer_);
  if (!renderer) {
    return false;
  }
  const Uint32 pixel_format = SdlPixelFormat(frame.format());
  if (pixel_format == SDL_PIXELFORMAT_UNKNOWN) {
    return false;
  }
  const SDL_Rect bounds{0, 0, frame.coded_size().width,
                        frame.coded_size().height};
  // (Re)create the streaming texture when the coded size changed (Δ3's
  // resolution change arrives here as a new coded size, not an event).
  if (!texture_ || texture_width_ != bounds.w || texture_height_ != bounds.h) {
    if (texture_) {
      SDL_DestroyTexture(static_cast<SDL_Texture*>(texture_));
      texture_ = nullptr;
    }
    texture_ =
        SDL_CreateTexture(renderer, pixel_format, SDL_TEXTUREACCESS_STREAMING,
                          bounds.w, bounds.h);
    texture_width_ = bounds.w;
    texture_height_ = bounds.h;
    if (!texture_) {
      return false;
    }
  }
  SDL_Texture* texture = static_cast<SDL_Texture*>(texture_);
  const Uint8* y = frame.visible_data(VideoFrame::kYPlane).data();
  const Uint8* u = frame.visible_data(VideoFrame::kUPlane).data();
  const Uint8* v = frame.visible_data(VideoFrame::kVPlane).data();
  if (!y || !u || !v) {
    return false;
  }
  // Time the same three stages the GL path reports, so the two backends stay
  // comparable: SDL_UpdateYUVTexture is the copy into the GPU-visible texture
  // (upload), the clear/copy/overlay triple is the draw (compose), and
  // SDL_RenderPresent is the swap (present).
  //
  // Read compose_ms here as "command submission", not "drawing": SDL queues
  // these and flushes them inside SDL_RenderPresent, so part of what the GL
  // path charges to compose lands in present_ms instead. The split is not
  // identical across backends, but neither number is wrong -- each measures
  // the span its own API defines.
  //
  // blit_ms stays zero on purpose, and it is the one stage where zero is the
  // right answer rather than a missing measurement: this path draws straight
  // to the window, so there is no off-screen canvas to round trip through.
  // Reporting nothing at all -- which is what this path used to do -- read as
  // "every stage is free", and that is the failure mode the fields exist to
  // prevent.
  const base::TimeTicks t_upload_begin = base::TimeTicks::Now();
  const int y_pitch = frame.stride(VideoFrame::kYPlane);
  const int u_pitch = frame.stride(VideoFrame::kUPlane);
  const int v_pitch = frame.stride(VideoFrame::kVPlane);
  int updated = -1;
  if (pixel_format == SDL_PIXELFORMAT_NV12) {
    updated = SDL_UpdateNVTexture(texture, &bounds, y, y_pitch, u, u_pitch);
  } else {
    // IYUV wants Y, U, V; YV12 wants Y, V, U -- the planes are swapped here,
    // the pixel format tells SDL which order the texture stores.
    const Uint8* second = pixel_format == SDL_PIXELFORMAT_YV12 ? v : u;
    const int second_pitch =
        pixel_format == SDL_PIXELFORMAT_YV12 ? v_pitch : u_pitch;
    const Uint8* third = pixel_format == SDL_PIXELFORMAT_YV12 ? u : v;
    const int third_pitch =
        pixel_format == SDL_PIXELFORMAT_YV12 ? u_pitch : v_pitch;
    updated = SDL_UpdateYUVTexture(texture, &bounds, y, y_pitch, second,
                                   second_pitch, third, third_pitch);
  }
  if (updated != 0) {
    return false;
  }
  const base::TimeTicks t_upload_end = base::TimeTicks::Now();
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, nullptr, nullptr);
  PresentOverlay(renderer_);
  const base::TimeTicks t_compose_end = base::TimeTicks::Now();
  SDL_RenderPresent(renderer);
  const base::TimeTicks t_present_end = base::TimeTicks::Now();

  if (timing) {
    timing->upload_ms = (t_upload_end - t_upload_begin).InMillisecondsF();
    timing->compose_ms = (t_compose_end - t_upload_end).InMillisecondsF();
    timing->present_ms = (t_present_end - t_compose_end).InMillisecondsF();
    // Straight to the window, so the CPU frame we just handed SDL is the route.
    timing->upload_path = UploadPath::kCpuSync;
  }
  return true;
}

void Sdl2OutputTarget::ReleaseOutput() {
  if (texture_) {
    SDL_DestroyTexture(static_cast<SDL_Texture*>(texture_));
    texture_ = nullptr;
  }
  if (overlay_texture_) {
    SDL_DestroyTexture(static_cast<SDL_Texture*>(overlay_texture_));
    overlay_texture_ = nullptr;
  }
  texture_width_ = 0;
  texture_height_ = 0;
  overlay_width_ = 0;
  overlay_height_ = 0;
  last_overlay_version_ = -1;
  renderer_ = nullptr;
  gl_context_ = nullptr;
  overlay_slot_ = nullptr;
  raw_ = nullptr;
}

// The SDL_Renderer overlay pass: uploads the slot's bitmap into a streaming
// RGBA texture when it changed and blends it at the host-published rect.
// Mirrors the GL path's semantics so both backends composite identically.
void Sdl2OutputTarget::PresentOverlay(void* renderer) {
  SDL_Renderer* sdl_renderer = static_cast<SDL_Renderer*>(renderer);
  if (!overlay_slot_) {
    return;
  }
  // Read the version before copying the bitmap: SnapshotTo() moves the whole
  // RGBA block under the writer's lock, and the common case is that nothing
  // was published since the last frame.
  const int version = overlay_slot_->published_version();
  if (version != snapshot_version_) {
    overlay_slot_->SnapshotTo(overlay_);
    snapshot_version_ = version;
  }
  const gl::TextOverlay& overlay = overlay_;
  if (!overlay.valid ||
      overlay.rgba.size() != static_cast<size_t>(overlay.width) *
                                 static_cast<size_t>(overlay.height) * 4u) {
    if (overlay_texture_) {
      SDL_DestroyTexture(static_cast<SDL_Texture*>(overlay_texture_));
      overlay_texture_ = nullptr;
      last_overlay_version_ = -1;
    }
    return;
  }
  if (!overlay_texture_ || last_overlay_version_ != overlay.version ||
      overlay_width_ != overlay.width || overlay_height_ != overlay.height) {
    if (overlay_texture_) {
      SDL_DestroyTexture(static_cast<SDL_Texture*>(overlay_texture_));
    }
    overlay_texture_ = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ABGR8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         overlay.width, overlay.height);
    if (!overlay_texture_) {
      return;
    }
    SDL_SetTextureBlendMode(static_cast<SDL_Texture*>(overlay_texture_),
                            SDL_BLENDMODE_BLEND);
    overlay_width_ = overlay.width;
    overlay_height_ = overlay.height;
    last_overlay_version_ = overlay.version;
    SDL_UpdateTexture(static_cast<SDL_Texture*>(overlay_texture_), nullptr,
                      overlay.rgba.data(), overlay.width * 4);
  }
  int out_w = 0;
  int out_h = 0;
  SDL_GetRendererOutputSize(sdl_renderer, &out_w, &out_h);
  // out_w/out_h are int and the rect fields are float, so the products below
  // were int-to-float conversions the compiler inserts silently; spelled out.
  const float out_wf = static_cast<float>(out_w);
  const float out_hf = static_cast<float>(out_h);
  const SDL_Rect dest{static_cast<int>(overlay.x * out_wf),
                      static_cast<int>(overlay.y * out_hf),
                      static_cast<int>(overlay.w * out_wf),
                      static_cast<int>(overlay.h * out_hf)};
  SDL_RenderCopy(sdl_renderer, static_cast<SDL_Texture*>(overlay_texture_),
                 nullptr, &dest);
}

}  // namespace avbase::media
