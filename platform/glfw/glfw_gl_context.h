// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_GLFW_GLFW_GL_CONTEXT_H_
#define AVBASE_PLATFORM_GLFW_GLFW_GL_CONTEXT_H_

#include "render/gl/native_gl_context.h"

typedef struct GLFWwindow GLFWwindow;

namespace avbase::media {

// NativeGLContext backed by GLFW. Wraps a host-owned GLFWwindow* (whose GL
// context is current on the calling thread). Borrowed semantics -- the
// destructor does NOT destroy the host's window/context.
class GlfwGLContext final : public gl::NativeGLContext {
 public:
  explicit GlfwGLContext(void* window);
  ~GlfwGLContext() override = default;

  void* GetProcAddress(const char* name) override;
  bool MakeCurrent() override;
  void ReleaseCurrent() override;
  void SwapBuffers() override;
  void GetDrawableSize(int* width, int* height) override;

 private:
  GLFWwindow* window_;
};

}  // namespace avbase::media

#endif  // AVBASE_PLATFORM_GLFW_GLFW_GL_CONTEXT_H_
