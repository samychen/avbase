// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_GL_GL_PRESENT_H_
#define AVBASE_MEDIA_GL_GL_PRESENT_H_

#include <memory>

#include "media/base/video_frame.h"
#include "media/media_export.h"
#include "render/gl/gl_api.h"
#include "render/gl/gl_program.h"
#include "render/gl/gl_quad.h"
#include "render/gl/gl_texture.h"
#include "render/gl/native_gl_context.h"
#include "render/gl/overlay.h"

namespace avbase::media {

// Uploads a planar YUV frame into three textures and draws it through a
// YUV->RGB program, optionally compositing a host-published overlay.
//
// This is the video-domain half of the GL path: it owns the colour matrices,
// the plane splitting and the NV12/YV12/I420 differences. Everything that is
// not video -- context abstraction, entry-point loading, program compilation,
// textures, the unit quad, the overlay slot -- comes from avbase_gl
// (render/gl), which knows nothing about frames. The split exists so that
// avbase_gl can be used by a project that has never heard of VideoFrame.
//
// Window-system neutral: it holds a gl::NativeGLContext (SDL2 / GLFW / EGL /
// ...) and never names a window-system SDK. Each backend owns the context
// lifetime and hands it in, so the same YUV->RGB path runs unchanged over any
// of them.
//
// THREADING: Init()/Present()/Shutdown() run on the sink's render thread;
// Init() makes the context current there and Shutdown() may be called from
// the host thread (it re-makes the context current before deleting). The
// host must not make the context current anywhere else.
class AVBASE_MEDIA_EXPORT GlPresenter {
 public:
  // Takes ownership of |ctx| (a borrowed wrapper over the host's context:
  // the wrapper does not destroy the host's window/context on destruction).
  explicit GlPresenter(std::unique_ptr<gl::NativeGLContext> ctx);
  GlPresenter(const GlPresenter&) = delete;
  GlPresenter& operator=(const GlPresenter&) = delete;
  ~GlPresenter();

  // Makes the context current on the calling thread, loads the GL entry
  // points and builds the programs. Must be the first call.
  bool Init();

  // Source for the overlay pass; may be null (no overlay) and may be called
  // before or after Init(). Borrowed: the host keeps the slot alive for the
  // sink's lifetime.
  void SetOverlaySource(gl::TextOverlaySlot* slot) { overlay_slot_ = slot; }

  // Uploads |frame| (kI420 / kYV12 / kNV12) and presents it. Returns false
  // when the format or geometry is not drawable; the caller reports a submit
  // failure. Honors the frame's colorspace/range by uploading the matching
  // conversion matrix.
  bool Present(const VideoFrame& frame);

  // Destroys GL objects. Safe to call twice; after the sink's render thread
  // has stopped, this runs on the host thread and re-makes the context
  // current first.
  void Shutdown();

  // The loaded entry points, for callers that want to log the driver's
  // renderer/version or gate an optional feature. Null before Init().
  const gl::GlApi* api() const { return api_.get(); }

 private:
  bool BuildPrograms();
  // (Re)creates the three plane textures when the coded size or format moved.
  bool EnsureTextures(const VideoFrame& frame);
  void UploadPlanes(const VideoFrame& frame);
  // Uploads uMatrix / uOffset from the frame's declared colorspace. CPU-side,
  // in floats, for the same reason video_convert.cc selects matrices
  // explicitly: a hard-wired BT.601 kernel is wrong for BT.709 HD sources.
  void UpdateColorUniforms(const VideoFrame& frame);
  // (Re)creates the overlay texture when the slot published a new version.
  bool EnsureOverlayTexture(const gl::TextOverlay& overlay);
  // Second draw pass: alpha-blends the slot's current bitmap over the video.
  // A no-op without a slot or with nothing published.
  void DrawOverlay();

  std::unique_ptr<gl::NativeGLContext>
      gl_ctx_;  // Borrowed wrapper, owned here.
  std::unique_ptr<gl::GlApi> api_;
  bool gl_current_{false};

  std::unique_ptr<gl::GlProgram> program_;          // Video pass.
  std::unique_ptr<gl::GlProgram> overlay_program_;  // Overlay pass.
  std::unique_ptr<gl::GlQuad> quad_;
  std::unique_ptr<gl::GlTexture> planes_[3];
  std::unique_ptr<gl::GlTexture> overlay_texture_;

  gl::TextOverlaySlot* overlay_slot_{nullptr};  // Borrowed; may be null.
  int overlay_version_{-1};
  int overlay_width_{0};
  int overlay_height_{0};
  int texture_width_{0};
  int texture_height_{0};
  int texture_format_{0};  // VideoFormat as int; 0 = kUnknown, i.e. none.
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_PRESENT_H_
