// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/glfw/glfw_video_sink.h"

#include <memory>

#include "platform/glfw/glfw_gl_context.h"
#include "platform/glfw/glfw_surface.h"

namespace avbase::media {

GlfwVideoSink::GlfwVideoSink(base::scoped_refptr<NativeDisplay> display)
    : GlVideoSink(std::move(display), NativeDisplayKind::kGlfwWindow, "glfw") {}

GlSurfaceBinding GlfwVideoSink::BindSurface(void* raw) {
  GlSurfaceBinding binding;
  auto* surface = static_cast<GlfwSurface*>(raw);
  binding.context = std::make_unique<GlfwGLContext>(surface->window);
  binding.overlay = surface->overlay;
  return binding;
}

std::unique_ptr<VideoRendererSink>
GlfwVideoSinkFactory::Create(base::scoped_refptr<NativeDisplay> display) {
  return std::make_unique<GlfwVideoSink>(std::move(display));
}

}  // namespace avbase::media
