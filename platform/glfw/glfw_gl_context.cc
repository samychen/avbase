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
  // Ask for a swap interval of 1 explicitly instead of inheriting GLFW's
  // default. That default is 0, but what the platform does with it differs --
  // CGL happens to vsync anyway, X11 does not -- which is why the same build
  // measured 60 fps on one run and 53 on the next: with no vsync the sink
  // falls back to its timer, and a timer is only good to a millisecond or two.
  // Setting it here makes the refresh the throttle everywhere, which is also
  // what lets GlVideoSink::PresentOne() recognize a blocking swap.
  glfwSwapInterval(1);
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
