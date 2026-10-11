// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_EGL_EGL_VIDEO_SINK_H_
#define AVBASE_PLATFORM_EGL_EGL_VIDEO_SINK_H_

#include "media/base/video_renderer_sink.h"
#include "render/gl/gl_video_sink.h"

namespace avbase::media {

// EGL video output endpoint (docs/archive/09 §2 embedding mode): the host owns
// the EGLDisplay / EGLSurface / EGLContext; this sink borrows them through
// NativeDisplay::FromEglSurface() and presents via GlPresenter. Like GLFW,
// EGL is GL-only, so the shared GlVideoSink loop is all it needs.
class AVBASE_MEDIA_EXPORT EglVideoSink final : public GlVideoSink {
 public:
  explicit EglVideoSink(base::scoped_refptr<NativeDisplay> display);
  EglVideoSink(const EglVideoSink&) = delete;
  EglVideoSink& operator=(const EglVideoSink&) = delete;
  ~EglVideoSink() override = default;

 protected:
  GlSurfaceBinding BindSurface(void* raw) override;
};

class AVBASE_MEDIA_EXPORT EglVideoSinkFactory final
    : public VideoRendererSinkFactory {
 public:
  std::unique_ptr<VideoRendererSink>
  Create(base::scoped_refptr<NativeDisplay> display) override;
  const char* name() const override { return "egl"; }
};

}  // namespace avbase::media

#endif  // AVBASE_PLATFORM_EGL_EGL_VIDEO_SINK_H_
