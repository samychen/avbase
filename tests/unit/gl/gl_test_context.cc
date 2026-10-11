// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gl_test_context.h"

namespace avbase::test {
namespace {

// glfwInit() is process-global and not reference-counted; doing it once and
// never terminating keeps two fixtures in the same binary from pulling the
// platform out from under each other.
bool InitGlfwOnce(std::string* error) {
  static const bool kInitialized = [] {
    return glfwInit() == GLFW_TRUE;
  }();
  if (!kInitialized && error) {
    *error = "glfwInit failed";
  }
  return kInitialized;
}

}  // namespace

GlTestWindow::~GlTestWindow() {
  // The context wrapper is borrowed-semantics, so it must go before the window
  // it wraps.
  context_.reset();
  if (window_) {
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }
}

std::unique_ptr<GlTestWindow> GlTestWindow::Create(int width, int height,
                                                   std::string* error) {
  if (!InitGlfwOnce(error)) {
    return nullptr;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  // The two hints that make this a test fixture rather than a player: no
  // visible window, and no swap interval, so a present cannot block on a
  // refresh the test is not trying to observe.
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  GLFWwindow* window =
      glfwCreateWindow(width, height, "avbase gl test", nullptr, nullptr);
  if (!window) {
    if (error) {
      *error = "glfwCreateWindow failed (no GL 3.3 core context on this host)";
    }
    return nullptr;
  }

  std::unique_ptr<GlTestWindow> self(new GlTestWindow());
  self->window_ = window;
  // GlfwGLContext's MakeCurrent() calls glfwMakeContextCurrent and sets the
  // swap interval to 1; override that back to 0 here so the swap is never the
  // thing under test.
  self->context_ = std::make_unique<media::GlfwGLContext>(window);
  if (!self->context_->MakeCurrent()) {
    if (error) {
      *error = "glfwMakeContextCurrent failed";
    }
    return nullptr;
  }
  glfwSwapInterval(0);
  // Report the size the driver actually gave us, which is what GetDrawableSize
  // will report later; a HiDPI scale factor would otherwise make the test's
  // expectations disagree with the presenter's viewport.
  glfwGetFramebufferSize(window, &self->width_, &self->height_);
  return self;
}

}  // namespace avbase::test
