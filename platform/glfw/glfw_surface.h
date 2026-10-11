// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_GLFW_GLFW_SURFACE_H_
#define AVBASE_PLATFORM_GLFW_GLFW_SURFACE_H_

#include "render/gl/overlay.h"

// The payload carried through NativeDisplay::FromGlfwWindow() for the GLFW
// backend. Embedding mode (docs/09 §2): the HOST owns the GLFWwindow
// and its GL context, and keeps this struct alive for as long as it passes the
// display to avbase; the sink only ever draws into it.
//
// GLFW ties the GL context to the window (there is no separate context
// handle), so the single |window| member is enough for GlfwGLContext to drive
// presentation. The host must create the window GL-capable (GLFW_OPENGL_*
// hints) and make the context current at least once so GlRuntimeWorks-style
// probing can run before playback starts.
struct GlfwSurface {
  void* window{nullptr};              // GLFWwindow*, host-owned.
  TextOverlaySlot* overlay{nullptr};  // Optional; composited over each frame.
};

#endif  // AVBASE_PLATFORM_GLFW_GLFW_SURFACE_H_
