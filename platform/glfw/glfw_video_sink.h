// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_GLFW_GLFW_VIDEO_SINK_H_
#define AVBASE_PLATFORM_GLFW_GLFW_VIDEO_SINK_H_

#include "media/base/video_renderer_sink.h"
#include "media/gl/gl_output_target.h"
#include "media/gl/gl_video_sink.h"
#include "media/media_export.h"

namespace avbase::media {

// The GLFW output target (docs/archive/09 §2 embedding mode): the host owns the
// GLFWwindow and its GL context, and this hands the sink a context adapter over
// them. GLFW is a GL-only backend, so has_gl_route() is unconditional -- the
// distinction exists for SDL2, which can be on either side of it.
class AVBASE_MEDIA_EXPORT GlfwOutputTarget final : public GlOutputTarget {
 public:
  void SetSurface(void* raw) override { raw_ = raw; }
  bool has_gl_route() const override { return true; }
  GlSurfaceBinding BindSurface() override;
  // Never called: has_gl_route() is always true.
  bool PresentWithoutGl(const VideoFrame& frame,
                        PresentTiming* timing) override;
  void ReleaseOutput() override { raw_ = nullptr; }

 private:
  // Untyped in the header so this file needs no GLFW include path.
  void* raw_{nullptr};
};

// GLFW video output endpoint: a GlfwOutputTarget behind the shared GlVideoSink
// loop. Every method that matters lives there; see Sdl2VideoSink's comment for
// why a backend is a target and not a second sink.
class AVBASE_MEDIA_EXPORT GlfwVideoSink final : public GlVideoSink {
 public:
  explicit GlfwVideoSink(base::scoped_refptr<NativeDisplay> display);
  GlfwVideoSink(const GlfwVideoSink&) = delete;
  GlfwVideoSink& operator=(const GlfwVideoSink&) = delete;
  ~GlfwVideoSink() override = default;
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
