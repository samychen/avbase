// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/glfw/glfw_video_sink.h"

#include <memory>

#include "platform/glfw/glfw_gl_context.h"
#include "platform/glfw/glfw_surface.h"
#include "platform/hwimport/hw_importer_factory.h"

namespace avbase::media {

GlfwVideoSink::GlfwVideoSink(base::scoped_refptr<NativeDisplay> display)
    : GlVideoSink(std::move(display), NativeDisplayKind::kGlfwWindow, "glfw") {
  // The host's zero-copy backend, built here rather than at BindSurface(): the
  // decision to read a hardware frame back has to be made BEFORE there is a
  // context to import through (see GlVideoSink::PresentOne), so the sink has
  // to know the answer from the moment it exists. Null on a host with no
  // interop backend, which is a legal answer.
  SetHwTextureImporter(platform::hwimport::CreateHwTextureImporter());
}

GlSurfaceBinding GlfwVideoSink::BindSurface(void* raw) {
  GlSurfaceBinding binding;
  auto* surface = static_cast<GlfwSurface*>(raw);
  binding.context = std::make_unique<GlfwGLContext>(surface->window);
  binding.overlay = surface->overlay;
  binding.canvas_width = surface->canvas_width;
  binding.canvas_height = surface->canvas_height;
  return binding;
}

std::unique_ptr<VideoRendererSink>
GlfwVideoSinkFactory::Create(base::scoped_refptr<NativeDisplay> display) {
  return std::make_unique<GlfwVideoSink>(std::move(display));
}

}  // namespace avbase::media
