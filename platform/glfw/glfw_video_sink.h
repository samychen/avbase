// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_GLFW_GLFW_VIDEO_SINK_H_
#define AVBASE_PLATFORM_GLFW_GLFW_VIDEO_SINK_H_

#include "media/base/video_renderer_sink.h"
#include "render/gl/gl_video_sink.h"

namespace avbase::media {

// GLFW video output endpoint (docs/archive/09 §2 embedding mode): the host owns
// the GLFWwindow and its GL context; this sink borrows them through
// NativeDisplay::FromGlfwWindow() and presents via GlPresenter. GLFW is a
// GL-only backend, so there is no 2D-renderer path -- the shared GlVideoSink
// loop is all it needs.
class AVBASE_MEDIA_EXPORT GlfwVideoSink final : public GlVideoSink {
 public:
  explicit GlfwVideoSink(base::scoped_refptr<NativeDisplay> display);
  GlfwVideoSink(const GlfwVideoSink&) = delete;
  GlfwVideoSink& operator=(const GlfwVideoSink&) = delete;
  ~GlfwVideoSink() override = default;

 protected:
  GlSurfaceBinding BindSurface(void* raw) override;
};

class AVBASE_MEDIA_EXPORT GlfwVideoSinkFactory final
    : public VideoRendererSinkFactory {
 public:
  std::unique_ptr<VideoRendererSink>
  Create(base::scoped_refptr<NativeDisplay> display) override;
  const char* name() const override { return "glfw"; }
};

}  // namespace avbase::media

#endif  // AVBASE_PLATFORM_GLFW_GLFW_VIDEO_SINK_H_
