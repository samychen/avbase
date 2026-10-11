// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/egl/egl_gl_context.h"

#include <EGL/egl.h>

#include <string>

namespace avbase::media {

EglGLContext::EglGLContext(void* display, void* surface, void* context)
    : display_(display), surface_(surface), context_(context) {}

void* EglGLContext::GetProcAddress(const char* name) {
  return reinterpret_cast<void*>(eglGetProcAddress(name));
}

bool EglGLContext::MakeCurrent() {
  if (eglMakeCurrent(static_cast<EGLDisplay>(display_),
                     static_cast<EGLSurface>(surface_),
                     static_cast<EGLSurface>(surface_),
                     static_cast<EGLContext>(context_)) != EGL_TRUE) {
    return false;
  }
  // Same reasoning as the GLFW and SDL2 contexts: request vsync explicitly so
  // the refresh, not the sink's timer, paces presentation. eglSwapInterval is
  // only honored while a context is current, which is why it lives here.
  eglSwapInterval(static_cast<EGLDisplay>(display_), 1);
  return true;
}

void EglGLContext::ReleaseCurrent() {
  eglMakeCurrent(static_cast<EGLDisplay>(display_), EGL_NO_SURFACE,
                 EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void EglGLContext::SwapBuffers() {
  eglSwapBuffers(static_cast<EGLDisplay>(display_),
                 static_cast<EGLSurface>(surface_));
}

void EglGLContext::GetDrawableSize(int* width, int* height) {
  EGLint w = 0;
  EGLint h = 0;
  eglQuerySurface(static_cast<EGLDisplay>(display_),
                  static_cast<EGLSurface>(surface_), EGL_WIDTH, &w);
  eglQuerySurface(static_cast<EGLDisplay>(display_),
                  static_cast<EGLSurface>(surface_), EGL_HEIGHT, &h);
  if (width) {
    *width = static_cast<int>(w);
  }
  if (height) {
    *height = static_cast<int>(h);
  }
}

std::string EglGLContext::GetError() {
  return "egl-error-0x" + std::to_string(static_cast<int>(eglGetError()));
}

}  // namespace avbase::media
