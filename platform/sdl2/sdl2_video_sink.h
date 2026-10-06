// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_SDL2_SDL2_VIDEO_SINK_H_
#define AVBASE_PLATFORM_SDL2_SDL2_VIDEO_SINK_H_

#include <atomic>
#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "media/base/native_display.h"
#include "platform/sdl2/surface.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/media_export.h"

namespace avbase::media {

class GlPresenter;

// The SDL2 video output endpoint: SDL_Renderer + SDL_Texture, driven from a
// dedicated render thread at the display cadence. When the host handed a GL
// context in through Sdl2Surface (see surface.h), presentation goes through
// GlPresenter's YUV→RGB shader path instead -- same thread, same cadence,
// same stats.
//
// THREADING (see surface.h): the host created the window/renderer on its main
// thread; this sink then uses the renderer only from its own thread, so no
// SDL video object is touched from two threads. With
// SDL_RENDERER_PRESENTVSYNC, SDL_RenderPresent blocks on the render thread,
// which is what couples presentation to the real refresh rate (Δ18).
//
// A display whose kind is not kSdl2 -- including NativeDisplay::None() --
// puts the sink in discard mode: Render() is still driven (the compositor
// needs the cadence and the video clock needs the presents) but nothing is
// uploaded.
class AVBASE_MEDIA_EXPORT Sdl2VideoSink final : public VideoRendererSink {
 public:
  explicit Sdl2VideoSink(base::scoped_refptr<NativeDisplay> display);
  Sdl2VideoSink(const Sdl2VideoSink&) = delete;
  Sdl2VideoSink& operator=(const Sdl2VideoSink&) = delete;
  ~Sdl2VideoSink() override;

  // VideoRendererSink:
  void Initialize(RenderCallback* callback) override;
  void Start() override;
  void Stop() override;
  void Pause() override;
  void Play() override;
  void Flush() override;
  void SetOutputTarget(base::scoped_refptr<NativeDisplay> display) override;
  bool IsRunning() const override;
  bool GetDisplayInterval(base::TimeDelta* interval) const override;
  VideoSinkStats GetStats() const override;
  const char* name() const override { return "Sdl2VideoSink"; }

 private:
  void PresentOne();
  // SDL_Renderer-path overlay pass; see Present()'s GL counterpart.
  void PresentOverlay(void* renderer);  // SDL_Renderer*.
  // Uploads |frame| into the sink-owned streaming texture and presents it.
  // Returns false when the frame format is not one the sink draws.
  bool UploadAndPresent(const VideoFrame& frame);

  base::scoped_refptr<NativeDisplay> display_;
  base::raw_ptr<RenderCallback> callback_{nullptr};
  std::unique_ptr<base::Thread> thread_;
  base::scoped_refptr<base::SequencedTaskRunner> render_runner_;

  // SDL types kept as void* in the header: platform/sdl2 may name SDL, but
  // every file that includes this one would then need the SDL include path,
  // and only the .cc does.
  void* renderer_{nullptr};
  void* texture_{nullptr};
  int texture_width_{0};
  int texture_height_{0};
  // SDL_Renderer-path overlay state (mirrors the GL presenter's).
  void* overlay_texture_{nullptr};
  int overlay_width_{0};
  int overlay_height_{0};
  int last_overlay_version_{-1};
  // Set when the host supplied an SDL_GLContext; owns the shader path.
  void* gl_context_{nullptr};
  std::unique_ptr<GlPresenter> presenter_;
  // Borrowed from the display's Sdl2Surface; may be null.
  TextOverlaySlot* overlay_slot_{nullptr};

  // Cross-thread flags: written from any thread (Pause/Play/SetOutputTarget
  // are callable anywhere), read on the render thread.
  std::atomic<bool> playing_{false};
  std::atomic<bool> discard_{false};
  std::atomic<uint64_t> frames_presented_{0};
  std::atomic<uint64_t> frames_dropped_{0};
  std::atomic<uint64_t> submit_failures_{0};
};

class AVBASE_MEDIA_EXPORT Sdl2VideoSinkFactory final
    : public VideoRendererSinkFactory {
 public:
  std::unique_ptr<VideoRendererSink>
  Create(base::scoped_refptr<NativeDisplay> display) override;
  const char* name() const override { return "sdl2"; }
};

}  // namespace avbase::media

#endif  // AVBASE_PLATFORM_SDL2_SDL2_VIDEO_SINK_H_
