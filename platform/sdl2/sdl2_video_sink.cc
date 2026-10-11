// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/sdl2/sdl2_video_sink.h"

#include <atomic>
#include <memory>
#include <utility>

#include "SDL.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "media/base/render_cadence.h"
#include "media/gl/gl_present.h"
#include "platform/hwimport/hw_importer_factory.h"
#include "platform/sdl2/sdl_gl_context.h"
#include "platform/sdl2/surface.h"

namespace avbase::media {
namespace {

// The sink draws 8-bit planar YUV (what sws_scale produces for I420 output);
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

Sdl2VideoSink::Sdl2VideoSink(base::scoped_refptr<NativeDisplay> display)
    : display_(std::move(display)) {
  discard_.store(!display_ ||
                 display_->kind() != NativeDisplayKind::kSdl2Window ||
                 !display_->raw());
  // The host's interop backend, installed here so this sink needs no
  // second wiring step beyond the one GlVideoSink exposes: on macOS this is
  // the same CGL/IOSurface importer the GLFW and EGL sinks get, because all
  // three end up on a CGL context.
  SetHwTextureImporter(platform::hwimport::CreateHwTextureImporter());
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
  // No cadence to inherit from a previous run; see GlVideoSink::Start().
  next_frame_deadline_ = base::TimeTicks();
  if (!discard_.load()) {
    const auto* surface = static_cast<const Sdl2Surface*>(display_->raw());
    renderer_ = surface->renderer;
    gl_context_ = surface->gl_context;
    overlay_slot_ = surface->overlay;
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
  // The presenter must die after the render thread is gone (its GL objects
  // belong to that thread's context) but before the host may destroy the
  // context it lent us. The scene driver goes first: it only drops a borrow,
  // and the presenter is what actually deletes the scene.
  scene_.Shutdown();
  presenter_.reset();
  if (texture_) {
    SDL_DestroyTexture(static_cast<SDL_Texture*>(texture_));
    texture_ = nullptr;
  }
  renderer_ = nullptr;
  gl_context_ = nullptr;
  overlay_slot_ = nullptr;
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
  // most one interval away. The GL context travels the same way: the next
  // present (re)builds the presenter for it, so drop the old one -- and the
  // scene, whose GL objects belong to that context -- here.
  scene_.Shutdown();
  presenter_.reset();
  if (display_ && display_->kind() == NativeDisplayKind::kSdl2Window &&
      display_->raw()) {
    const auto* surface = static_cast<const Sdl2Surface*>(display_->raw());
    // M5: renderer_ was only assigned in Start(), so a retarget to a surface
    // with a different (or null) SDL_Renderer kept presenting through the OLD
    // renderer until the next full Start/Stop cycle.
    renderer_ = surface->renderer;
    gl_context_ = surface->gl_context;
    overlay_slot_ = surface->overlay;
  } else {
    renderer_ = nullptr;
    gl_context_ = nullptr;
    overlay_slot_ = nullptr;
  }
}

bool Sdl2VideoSink::IsRunning() const {
  return thread_ != nullptr;
}

bool Sdl2VideoSink::GetDisplayInterval(base::TimeDelta* interval) const {
  if (interval) {
    *interval = kDefaultFrameInterval;
  }
  // An estimate, not a measured vsync: returning true makes the compositor
  // use this as its display window, which is closer to the truth than "no
  // information" for the common 60 Hz case.
  return true;
}

VideoSinkStats Sdl2VideoSink::GetStats() const {
  return stats_.Get();
}

void Sdl2VideoSink::NotePresentTiming(const PresentTiming& timing) {
  stats_.NotePresentTiming(timing);
}

void Sdl2VideoSink::PresentOne() {
  const base::TimeTicks frame_start = base::TimeTicks::Now();
  if (playing_.load() && callback_) {
    base::scoped_refptr<VideoFrame> frame =
        callback_->Render(base::TimeTicks(), base::TimeTicks());
    // Zero-copy contract (avbase §6.2): a hardware frame arrives GPU-resident.
    // Which of two things happens to it is a property of the PATH it is about
    // to take, not of the frame:
    //
    //   * the GL path, with an import backend installed, takes it as it is and
    //     the presenter points its textures at the decoded surface;
    //   * the SDL_Renderer path -- which uploads through SDL_UpdateYUVTexture
    //     and therefore needs the plane bytes in CPU memory -- reads it back,
    //     EXPLICITLY, here. That keeps the cost in exactly one place, and
    //     keeps hardware playback CORRECT on a host that cannot import:
    //     expensive, not broken.
    //
    // The gl_context_ term is the ONLY question this sink still asks itself.
    // Everything on the GL side of it -- import, or read back because there is
    // no backend, or read back because the driver refused -- is
    // GlVideoLayer::Upload()'s decision now, and asking "can I import" here as
    // well is what made this sink disagree with the GL sink about whether a
    // refused import was a dropped frame.
    bool readback_failed = false;
    if (frame && !frame->IsMappable() && !gl_context_) {
      base::scoped_refptr<VideoFrame> mapped = frame->ToI420();
      if (!mapped) {
        // No readback path installed. The frame is dropped here and the
        // outcome below reports it once, as a failure.
        frame = nullptr;
        readback_failed = true;
      } else {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
          LOG(WARNING) << "sdl2 sink read back a hardware frame via ToI420(); "
                          "expect this only on the CPU-fallback display path";
        }
        frame = std::move(mapped);
      }
    }
    const bool presented = !discard_.load() && frame && UploadAndPresent(frame);
    const FrameOutcome outcome = ClassifyFrameOutcome(
        frame != nullptr, discard_.load(), readback_failed, presented);
    if (outcome == FrameOutcome::kSubmitFailure) {
      callback_->OnFrameSubmitFailure();
    }
    stats_.NoteFrameOutcome(outcome);
  }
  if (render_runner_) {
    // Shared with GlVideoSink on purpose: two copies of a cadence rule means
    // one of them eventually keeps the bug. See NextFrameDelay for which of
    // the two regimes applies.
    const FramePacing pacing =
        NextFrameDelay(frame_start, base::TimeTicks::Now(),
                       next_frame_deadline_, kDefaultFrameInterval);
    next_frame_deadline_ = pacing.next_deadline;
    render_runner_->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Sdl2VideoSink::PresentOne, base::Unretained(this)),
        pacing.delay);
  }
}

void Sdl2VideoSink::SetSceneUpdate(GlSceneDriver::UpdateCallback callback) {
  scene_.SetUpdateCallback(std::move(callback));
}

void Sdl2VideoSink::SetHwTextureImporter(
    std::unique_ptr<GlHwTextureImporter> importer) {
  hw_importer_ = std::move(importer);
  LOG(INFO) << "Sdl2VideoSink hardware import backend: "
            << (hw_importer_ ? hw_importer_->name() : "none");
}

bool Sdl2VideoSink::UploadAndPresent(
    const base::scoped_refptr<VideoFrame>& frame) {
  // GL path: the host supplied a context with the display. The presenter is
  // created lazily here because Init() must run on the render thread (it
  // makes the context current there).
  if (gl_context_ && !presenter_) {
    const auto* surface = static_cast<const Sdl2Surface*>(display_->raw());
    auto presenter = std::make_unique<GlPresenter>(
        std::make_unique<SdlGLContext>(surface->window, gl_context_));
    presenter->SetOverlaySource(overlay_slot_);
    presenter->SetCanvasSize(surface->canvas_width, surface->canvas_height);
    presenter->SetHwTextureImporter(hw_importer_.get());
    if (!presenter->Init()) {
      // Context exists but the shader path is unusable (e.g. a GL 2.0-era
      // driver): report failure rather than silently degrade the picture.
      LOG(ERROR) << "gl presenter init failed";
      gl_context_ = nullptr;
      return false;
    }
    presenter_ = std::move(presenter);
  }
  if (presenter_) {
    PresentTiming timing;
    // One call either way: the driver forwards to Present() when the host
    // installed no scene hook, so the single-frame path is not a second branch
    // here that could drift from the scene one.
    if (scene_.Present(*presenter_, frame, &timing)) {
      NotePresentTiming(timing);
      return true;
    }
    // No second attempt here either: a frame the import route would not take
    // is one Upload() has already read back and retried inside the layer, so
    // retrying the whole scene from here would walk every layer again for one
    // that already had its answer.
    return false;
  }
  SDL_Renderer* renderer = static_cast<SDL_Renderer*>(renderer_);
  if (!renderer) {
    return false;
  }
  const Uint32 pixel_format = SdlPixelFormat(frame->format());
  if (pixel_format == SDL_PIXELFORMAT_UNKNOWN) {
    return false;
  }
  const SDL_Rect bounds{0, 0, frame->coded_size().width,
                        frame->coded_size().height};
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
  const Uint8* y = frame->visible_data(VideoFrame::kYPlane).data();
  const Uint8* u = frame->visible_data(VideoFrame::kUPlane).data();
  const Uint8* v = frame->visible_data(VideoFrame::kVPlane).data();
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
  const int y_pitch = frame->stride(VideoFrame::kYPlane);
  const int u_pitch = frame->stride(VideoFrame::kUPlane);
  const int v_pitch = frame->stride(VideoFrame::kVPlane);
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

  PresentTiming timing;
  timing.upload_ms = (t_upload_end - t_upload_begin).InMillisecondsF();
  timing.compose_ms = (t_compose_end - t_upload_end).InMillisecondsF();
  timing.present_ms = (t_present_end - t_compose_end).InMillisecondsF();
  // Straight to the window, so the CPU frame we just handed SDL is the route.
  timing.upload_path = UploadPath::kCpuSync;
  NotePresentTiming(timing);
  return true;
}

// The SDL_Renderer overlay pass: uploads the slot's bitmap into a streaming
// RGBA texture when it changed and blends it at the host-published rect.
// Mirrors the GL path's semantics so both backends composite identically.
void Sdl2VideoSink::PresentOverlay(void* renderer) {
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
  const SDL_Rect dest{
      static_cast<int>(overlay.x * out_w), static_cast<int>(overlay.y * out_h),
      static_cast<int>(overlay.w * out_w), static_cast<int>(overlay.h * out_h)};
  SDL_RenderCopy(sdl_renderer, static_cast<SDL_Texture*>(overlay_texture_),
                 nullptr, &dest);
}

std::unique_ptr<VideoRendererSink>
Sdl2VideoSinkFactory::Create(base::scoped_refptr<NativeDisplay> display) {
  return std::make_unique<Sdl2VideoSink>(std::move(display));
}

}  // namespace avbase::media
