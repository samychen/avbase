// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/sdl2/sdl2_video_sink.h"

#include "SDL.h"

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "platform/sdl2/surface.h"

#include <atomic>

namespace avbase::media {
namespace {

// Render cadence when the sink has no vsync source to wait on. With
// SDL_RENDERER_PRESENTVSYNC the Present below blocks for the real refresh, so
// this only bounds the *maximum* rate; without it, this is the rate.
constexpr base::TimeDelta kMaxPresentInterval = base::Milliseconds(1000) / 60;

// The sink draws 8-bit planar YUV (what sws_scale produces for I420 output);
// anything else is reported as a submit failure rather than drawn wrong.
Uint32 SdlPixelFormat(VideoFormat format) {
  switch (format) {
    case VideoFormat::kI420: return SDL_PIXELFORMAT_IYUV;
    case VideoFormat::kYV12: return SDL_PIXELFORMAT_YV12;
    case VideoFormat::kNV12: return SDL_PIXELFORMAT_NV12;
    default:                 return SDL_PIXELFORMAT_UNKNOWN;
  }
}

}  // namespace

Sdl2VideoSink::Sdl2VideoSink(base::scoped_refptr<NativeDisplay> display)
    : display_(std::move(display)) {
  discard_.store(!display_ ||
                 display_->kind() != NativeDisplayKind::kSdl2Window ||
                 !display_->raw());
}

Sdl2VideoSink::~Sdl2VideoSink() {
  Stop();
}

void Sdl2VideoSink::Initialize(RenderCallback* callback) {
  callback_ = callback;
}

void Sdl2VideoSink::Start() {
  if (thread_) {
    return;
  }
  if (!discard_.load()) {
    const auto* surface = static_cast<const Sdl2Surface*>(display_->raw());
    renderer_ = surface->renderer;
  }
  thread_ = std::make_unique<base::Thread>("avbase-sdl-video");
  thread_->Start();
  render_runner_ = thread_->task_runner();
  PresentOne();
}

void Sdl2VideoSink::Stop() {
  playing_.store(false);
  if (thread_) {
    // Join the render thread before touching the renderer: after Stop()
    // returns, PresentOne() can no longer be in flight (the contract the
    // sub-renderer's teardown ordering relies on).
    thread_->Stop();
    thread_.reset();
    render_runner_ = nullptr;
  }
  if (texture_) {
    SDL_DestroyTexture(static_cast<SDL_Texture*>(texture_));
    texture_ = nullptr;
  }
  renderer_ = nullptr;
}

void Sdl2VideoSink::Pause() {
  playing_.store(false);
}

void Sdl2VideoSink::Play() {
  playing_.store(true);
}

void Sdl2VideoSink::Flush() {
  // The compositor owns buffered frames; the texture keeps showing whatever
  // it showed, which is the right post-flush picture until the next present.
}

void Sdl2VideoSink::SetOutputTarget(
    base::scoped_refptr<NativeDisplay> display) {
  display_ = std::move(display);
  discard_.store(!display_ ||
                 display_->kind() != NativeDisplayKind::kSdl2Window ||
                 !display_->raw());
  // Pick up the new renderer on the next present; swapping mid-frame is not
  // possible without owning the render sequence, and the next tick is at
  // most one interval away.
}

bool Sdl2VideoSink::IsRunning() const {
  return thread_ != nullptr;
}

bool Sdl2VideoSink::GetDisplayInterval(base::TimeDelta* interval) const {
  if (interval) {
    *interval = kMaxPresentInterval;
  }
  // An estimate, not a measured vsync: returning true makes the compositor
  // use this as its display window, which is closer to the truth than "no
  // information" for the common 60 Hz case.
  return true;
}

VideoSinkStats Sdl2VideoSink::GetStats() const {
  VideoSinkStats stats;
  stats.frames_presented = frames_presented_.load();
  stats.frames_dropped = frames_dropped_.load();
  stats.submit_failures = submit_failures_.load();
  return stats;
}

void Sdl2VideoSink::PresentOne() {
  if (playing_.load() && callback_) {
    base::scoped_refptr<VideoFrame> frame =
        callback_->Render(base::TimeTicks(), base::TimeTicks());
    // Zero-copy contract (avbase §6.2): a hardware frame arrives GPU-resident
    // and SDL2's CPU blit cannot take it. The readback is EXPLICIT here --
    // ToI420() -- so the GPU path stays the default and the cost shows up in
    // exactly one place. A zero-copy SDL display path (NV12 texture import)
    // is what replaces this when it lands; until then hardware playback is
    // still correct, just with one readback per presented frame.
    if (frame && !frame->IsMappable()) {
      base::scoped_refptr<VideoFrame> mapped = frame->ToI420();
      if (!mapped) {
        // No readback path installed: treat as a submit failure so the
        // renderer's stats tell the truth instead of dropping silently.
        submit_failures_.fetch_add(1);
        frame = nullptr;
      } else {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
          LOG(WARNING) << "sdl2 sink read back a hardware frame via ToI420(); "
                          "expect this only on the CPU-fallback display path";
        }
        frame = std::move(mapped);
      }
    }
    if (discard_.load() || !frame) {
      frames_dropped_.fetch_add(1);
    } else if (UploadAndPresent(*frame)) {
      frames_presented_.fetch_add(1);
    } else {
      submit_failures_.fetch_add(1);
    }
  }
  if (render_runner_) {
    render_runner_->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Sdl2VideoSink::PresentOne, base::Unretained(this)),
        kMaxPresentInterval);
  }
}

bool Sdl2VideoSink::UploadAndPresent(const VideoFrame& frame) {
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
    texture_ = SDL_CreateTexture(renderer, pixel_format,
                                 SDL_TEXTUREACCESS_STREAMING, bounds.w,
                                 bounds.h);
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
    const int second_pitch = pixel_format == SDL_PIXELFORMAT_YV12 ? v_pitch
                                                                  : u_pitch;
    const Uint8* third = pixel_format == SDL_PIXELFORMAT_YV12 ? u : v;
    const int third_pitch = pixel_format == SDL_PIXELFORMAT_YV12 ? u_pitch
                                                                 : v_pitch;
    updated = SDL_UpdateYUVTexture(texture, &bounds, y, y_pitch, second,
                                   second_pitch, third, third_pitch);
  }
  if (updated != 0) {
    return false;
  }
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, nullptr, nullptr);
  SDL_RenderPresent(renderer);
  return true;
}

std::unique_ptr<VideoRendererSink> Sdl2VideoSinkFactory::Create(
    base::scoped_refptr<NativeDisplay> display) {
  return std::make_unique<Sdl2VideoSink>(std::move(display));
}

}  // namespace avbase::media
