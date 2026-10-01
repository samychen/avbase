// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_SDL2_SURFACE_H_
#define AVBASE_PLATFORM_SDL2_SURFACE_H_

// The payload carried through NativeDisplay::FromSdl2Window() for the SDL2
// backend. Embedding mode (docs/09 §2): the HOST owns the window and the
// renderer and keeps this struct alive for as long as it passes the display
// to avbase; the sink only ever draws into it.
//
// The window and the renderer must be created on the host's main thread
// (Cocoa requires it on macOS); the sink then uses the renderer exclusively
// from its own render thread, and the host sticks to SDL_PollEvent on the
// main thread. Those two rules are what keep every SDL object single-threaded.
struct Sdl2Surface {
  void* window{nullptr};    // SDL_Window*
  void* renderer{nullptr};  // SDL_Renderer*
};

#endif  // AVBASE_PLATFORM_SDL2_SURFACE_H_
