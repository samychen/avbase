// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_NATIVE_GL_CONTEXT_H_
#define AVBASE_RENDER_GL_NATIVE_GL_CONTEXT_H_

#include <string>

#include "render/gl/gl_export.h"

namespace avbase::gl {

// The seam between this module and whatever owns the window: a GL context plus
// the two things the module needs from its host -- a way to resolve entry
// points, and a way to bind the context to the calling thread.
//
// Each window system (SDL2 / GLFW / EGL / a future Metal or WGL bridge)
// supplies one implementation. Nothing in this module names a window-system SDK
// or a GL header: entry points arrive one name at a time through
// GetProcAddress, which is what lets the same shaders and draw calls run
// unchanged over any of them.
//
// Implementations are BORROWED-semantics wrappers: they do not own the
// underlying window/context (the host does, per the embedding rules in
// docs/archive/09 §2), so their destructor must not destroy the host's objects.
//
// THREADING: a GL context is current on at most one thread at a time. This
// module calls MakeCurrent() on its own render thread before touching GL and
// ReleaseCurrent() when it is done; a host that also binds the context
// elsewhere must expect the module's objects to be invalid there.
class AVBASE_GL_EXPORT NativeGLContext {
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

}  // namespace avbase::gl

#endif  // AVBASE_RENDER_GL_NATIVE_GL_CONTEXT_H_
