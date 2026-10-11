// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_NATIVE_GL_CONTEXT_H_
#define AVBASE_RENDER_GL_NATIVE_GL_CONTEXT_H_

#include <string>

namespace avbase::media {

// Platform-neutral GL context + proc-resolver abstraction.
//
// Each window system (SDL2 / GLFW / EGL / ...) supplies one implementation.
// The GL presenter and the loader talk ONLY to this interface, so the
// rendering code never names a window-system SDK -- the same void* erasure
// that NativeDisplay uses at the media<->platform boundary. This is what lets
// the YUV->RGB shader path run unchanged over SDL, GLFW or EGL.
//
// Implementations are BORROWED-semantics wrappers: they do not own the
// underlying window/context (the host does, per the embedding rules in
// docs/09), so their destructor must not destroy the host's objects.
class NativeGLContext {
 public:
  virtual ~NativeGLContext() = default;

  // Resolves a GL entry point by name; returns nullptr for unsupported
  // functions (old drivers). Mirrors SDL_GL_GetProcAddress semantics.
  virtual void* GetProcAddress(const char* name) = 0;

  // Binds the context to the calling thread. Returns false on failure.
  virtual bool MakeCurrent() = 0;

  // Releases the context from the calling thread. A no-op on backends that
  // don't need an explicit release.
  virtual void ReleaseCurrent() {}

  // Presents the back buffer (swap / eglSwapBuffers).
  virtual void SwapBuffers() = 0;

  // Drawable pixel size; used for the GL viewport.
  virtual void GetDrawableSize(int* width, int* height) = 0;

  // Last-error string ("" if none); used in logs.
  virtual std::string GetError() { return std::string(); }
};

}  // namespace avbase::media

#endif  // AVBASE_RENDER_GL_NATIVE_GL_CONTEXT_H_
