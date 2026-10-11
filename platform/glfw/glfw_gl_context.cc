// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/glfw/glfw_gl_context.h"

#include <GLFW/glfw3.h>

#include <string>

namespace avbase::media {

GlfwGLContext::GlfwGLContext(void* window)
    : window_(static_cast<GLFWwindow*>(window)) {}

void* GlfwGLContext::GetProcAddress(const char* name) {
  return reinterpret_cast<void*>(glfwGetProcAddress(name));
}

bool GlfwGLContext::MakeCurrent() {
  glfwMakeContextCurrent(window_);
  return true;
}

void GlfwGLContext::ReleaseCurrent() {
  glfwMakeContextCurrent(nullptr);
}

void GlfwGLContext::SwapBuffers() {
  glfwSwapBuffers(window_);
}

void GlfwGLContext::GetDrawableSize(int* width, int* height) {
  glfwGetFramebufferSize(window_, width, height);
}

}  // namespace avbase::media
