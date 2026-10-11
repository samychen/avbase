// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/egl/egl_video_sink.h"

#include <memory>

#include "platform/egl/egl_gl_context.h"
#include "platform/egl/egl_surface.h"
#include "platform/hwimport/hw_importer_factory.h"

namespace avbase::media {

EglVideoSink::EglVideoSink(base::scoped_refptr<NativeDisplay> display)
    : GlVideoSink(std::move(display), NativeDisplayKind::kEglWindow, "egl") {
  // Same contract as the GLFW backend: the backend is chosen by the HOST'S
  // GRAPHICS STACK, not by the window system, so an EGL host on macOS gets the
  // same CGL/IOSurface importer the GLFW one does (see platform/hwimport).
  SetHwTextureImporter(platform::hwimport::CreateHwTextureImporter());
}

GlSurfaceBinding EglVideoSink::BindSurface(void* raw) {
  GlSurfaceBinding binding;
  auto* surface = static_cast<EglSurface*>(raw);
  binding.context = std::make_unique<EglGLContext>(
      surface->display, surface->surface, surface->context);
  binding.overlay = surface->overlay;
  binding.canvas_width = surface->canvas_width;
  binding.canvas_height = surface->canvas_height;
  return binding;
}

std::unique_ptr<VideoRendererSink>
EglVideoSinkFactory::Create(base::scoped_refptr<NativeDisplay> display) {
  return std::make_unique<EglVideoSink>(std::move(display));
}

}  // namespace avbase::media
