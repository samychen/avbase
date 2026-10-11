// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/sdl2/sdl_gl_context.h"

#include <SDL.h>

#include <string>

namespace avbase::media {

SdlGLContext::SdlGLContext(void* window, void* context)
    : window_(window), context_(context) {}

void* SdlGLContext::GetProcAddress(const char* name) {
  return SDL_GL_GetProcAddress(name);
}

bool SdlGLContext::MakeCurrent() {
  return SDL_GL_MakeCurrent(static_cast<SDL_Window*>(window_),
                            static_cast<SDL_GLContext>(context_)) == 0;
}

void SdlGLContext::ReleaseCurrent() {
  SDL_GL_MakeCurrent(static_cast<SDL_Window*>(window_), nullptr);
}

void SdlGLContext::SwapBuffers() {
  SDL_GL_SwapWindow(static_cast<SDL_Window*>(window_));
}

void SdlGLContext::GetDrawableSize(int* width, int* height) {
  SDL_GL_GetDrawableSize(static_cast<SDL_Window*>(window_), width, height);
}

std::string SdlGLContext::GetError() {
  return std::string(SDL_GetError());
}

}  // namespace avbase::media
