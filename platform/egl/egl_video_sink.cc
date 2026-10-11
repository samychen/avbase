// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/egl/egl_video_sink.h"

#include <memory>

#include "platform/egl/egl_gl_context.h"
#include "platform/egl/egl_surface.h"

namespace avbase::media {

EglVideoSink::EglVideoSink(base::scoped_refptr<NativeDisplay> display)
    : GlVideoSink(std::move(display), NativeDisplayKind::kEglWindow, "egl") {}

GlSurfaceBinding EglVideoSink::BindSurface(void* raw) {
  GlSurfaceBinding binding;
  auto* surface = static_cast<EglSurface*>(raw);
  binding.context = std::make_unique<EglGLContext>(
      surface->display, surface->surface, surface->context);
  binding.overlay = surface->overlay;
  return binding;
}

std::unique_ptr<VideoRendererSink>
EglVideoSinkFactory::Create(base::scoped_refptr<NativeDisplay> display) {
  return std::make_unique<EglVideoSink>(std::move(display));
}

}  // namespace avbase::media
