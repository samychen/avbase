// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_SDL2_SURFACE_H_
#define AVBASE_PLATFORM_SDL2_SURFACE_H_

#include <cstdint>
#include <mutex>
#include <vector>

// A host-published overlay bitmap (subtitle cue, watermark, ...): tightly
// packed straight-alpha RGBA8, placed in fractions of the drawable size.
struct TextOverlay {
  bool valid{false};
  int version{0};  // Bumped on every publish; the sink re-uploads on change.
  int width{0};
  int height{0};
  std::vector<uint8_t> rgba;
  float x{0}, y{0}, w{0}, h{0};  // Fractions of the drawable size.
};

// The host owns the slot and mutates it through Publish()/Clear() from its
// own threads; the sink snapshots it once per presented frame on the render
// thread. The mutex is inside because the publish rate (per cue) and the
// snapshot rate (per frame) are on different threads by design.
struct TextOverlaySlot {
  void Publish(TextOverlay overlay) {
    std::lock_guard<std::mutex> lock(mu_);
    overlay.version = ++version_;
    current_ = std::move(overlay);
    current_.valid = true;
  }
  void Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    current_.valid = false;
  }
  // Copies the current overlay (or an invalid one when nothing is published).
  TextOverlay Snapshot() {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
  }

 private:
  std::mutex mu_;
  int version_{0};
  TextOverlay current_;
};

// The payload carried through NativeDisplay::FromSdl2Window() for the SDL2
// backend. Embedding mode (docs/09 §2): the HOST owns the window and the
// renderer and keeps this struct alive for as long as it passes the display
// to avbase; the sink only ever draws into it.
//
// The window and the renderer must be created on the host's main thread
// (Cocoa requires it on macOS); the sink then uses the renderer exclusively
// from its own render thread, and the host sticks to SDL_PollEvent on the
// main thread. Those two rules are what keep every SDL object single-threaded.
//
// When |gl_context| is set (an SDL_GLContext the host created for |window|,
// never made current on the host thread), the sink presents through the GL
// shader path in gl_present.h instead of SDL_Renderer, and |renderer| is
// unused. The context must be created with a GL 3.3 core profile; the sink
// makes it current on its render thread, so the host must not touch it after
// handing it over. |overlay|, when non-null, is composited over every
// presented frame on the GL path.
struct Sdl2Surface {
  void* window{nullptr};       // SDL_Window*
  void* renderer{nullptr};     // SDL_Renderer*
  void* gl_context{nullptr};   // SDL_GLContext, optional; selects the GL path.
  TextOverlaySlot* overlay{nullptr};  // Optional; GL path only.
};

#endif  // AVBASE_PLATFORM_SDL2_SURFACE_H_
