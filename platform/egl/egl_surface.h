// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_EGL_EGL_SURFACE_H_
#define AVBASE_PLATFORM_EGL_EGL_SURFACE_H_

#include "render/gl/overlay.h"

// The payload carried through NativeDisplay::FromEglSurface() for the EGL
// backend. Embedding mode (docs/archive/09 §2): the HOST owns the EGLDisplay /
// EGLSurface / EGLContext, and keeps this struct alive for as long as it
// passes the display to avbase; the sink only ever draws through them.
//
// EGL separates the three handles, so all three are carried here. On a typical
// Linux EGL setup the host builds these from a GBM device or an X11/ Wayland
// EGL display; EglGLContext forwards exactly the three it needs.
struct EglSurface {
  void* display{nullptr};                         // EGLDisplay, host-owned.
  void* surface{nullptr};                         // EGLSurface, host-owned.
  void* context{nullptr};                         // EGLContext, host-owned.
  avbase::gl::TextOverlaySlot* overlay{nullptr};  // Optional; per frame.
};

#endif  // AVBASE_PLATFORM_EGL_EGL_SURFACE_H_
