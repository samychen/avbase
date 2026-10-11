// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_TESTS_UNIT_GL_GL_TEST_CONTEXT_H_
#define AVBASE_TESTS_UNIT_GL_GL_TEST_CONTEXT_H_

#include <memory>
#include <string>
#include <utility>

// GLFW includes the platform's GL header by default, and glad's header refuses
// to be included after one ("OpenGL (gl.h) header already included"), which is
// what a test that uses both would otherwise hit. GLFW_INCLUDE_NONE tells it to
// include no GL header at all -- the GL types in this binary come from glad's
// own copy, never from Apple's 2.1-era <OpenGL/gl.h>. It has to be defined
// BEFORE glfw3.h is included to have any effect.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "platform/glfw/glfw_gl_context.h"
#include "render/gl/native_gl_context.h"

namespace avbase::test {

// A HIDDEN window with a GL 3.3 core context, current on the creating thread.
//
// Why pixels need a window at all: everything else in this tree can be tested
// against a return code, but "is the picture the right way up" cannot. The
// only honest check is to draw a frame whose top and bottom differ and read
// the result back, which needs a real context -- and a real context on this
// project's backends comes from a window system.
//
// It is HIDDEN because nothing here needs to reach the screen: the pixels are
// read out of the back buffer before any swap, so the window is only a
// container for the context. That also means the test can run on a host with
// no display, which is the closest a GL test gets to a headless CI.
//
// Context creation is allowed to fail on a host with no GL driver; callers
// turn that into GTEST_SKIP rather than a failure, and the reason travels in
// |error| so a skipped run still says why.
class GlTestWindow {
 public:
  GlTestWindow(const GlTestWindow&) = delete;
  GlTestWindow& operator=(const GlTestWindow&) = delete;
  ~GlTestWindow();

  // Returns null and fills |error| when this host cannot provide a GL 3.3 core
  // context. |width| x |height| is the drawable size the presenter will see.
  static std::unique_ptr<GlTestWindow> Create(int width, int height,
                                              std::string* error);

  gl::NativeGLContext* context() { return context_.get(); }

  // Hands the context wrapper to a GlPresenter, which owns it for the rest of
  // its life. Safe because the wrapper is borrowed-semantics: what moves is the
  // adapter over the window, not the window.
  std::unique_ptr<gl::NativeGLContext> TakeContext() {
    return std::move(context_);
  }

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  GlTestWindow() = default;

  GLFWwindow* window_{nullptr};
  std::unique_ptr<media::GlfwGLContext> context_;
  int width_{0};
  int height_{0};
};

}  // namespace avbase::test

#endif  // AVBASE_TESTS_UNIT_GL_GL_TEST_CONTEXT_H_
