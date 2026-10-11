// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_SDL2_SDL_GL_CONTEXT_H_
#define AVBASE_PLATFORM_SDL2_SDL_GL_CONTEXT_H_

#include "render/gl/native_gl_context.h"

namespace avbase::media {

// NativeGLContext backed by SDL2: wraps a host-owned SDL_Window* and
// SDL_GLContext. Borrowed semantics -- the destructor does NOT destroy the
// host's context (per the embedding rules in docs/09).
class SdlGLContext final : public NativeGLContext {
 public:
  SdlGLContext(void* window, void* context);
  ~SdlGLContext() override = default;

  void* GetProcAddress(const char* name) override;
  bool MakeCurrent() override;
  void ReleaseCurrent() override;
  void SwapBuffers() override;
  void GetDrawableSize(int* width, int* height) override;
  std::string GetError() override;

 private:
  void* window_;
  void* context_;
};

}  // namespace avbase::media

#endif  // AVBASE_PLATFORM_SDL2_SDL_GL_CONTEXT_H_
