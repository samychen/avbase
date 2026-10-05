// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_SDL2_GL_PRESENT_H_
#define AVBASE_PLATFORM_SDL2_GL_PRESENT_H_

#include <memory>

#include "media/base/video_frame.h"

namespace avbase::media {

// The GL presentation half of Sdl2VideoSink: uploads a planar YUV frame into
// three textures and draws it through a YUV→RGB shader, ported from
// MediaComponent's framework/render/gl programs (nv12_program /
// base_2d_program) onto the VideoRendererSink contract.
//
// The pieces of MediaComponent's GL stack that this deliberately does NOT
// port: GLEW (functions resolve through SDL_GL_GetProcAddress), the
// RenderPipeline/stage abstraction and the ping-pong FBO chain (a display
// sink is a single pass; the FBO machinery earns its keep only when a second
// pass -- text overlay, transitions -- lands).
//
// All types stay as void* in this header: only the .cc may name SDL or GL.
//
// THREADING: Init()/Present()/Shutdown() run on the sink's render thread;
// Init() makes the context current there and Shutdown() may be called from
// the host thread (it re-makes the context current before deleting). The
// host must not make the context current anywhere else.
class GlPresenter {
 public:
  // |window|: SDL_Window*; |context|: SDL_GLContext (GL 3.3 core, created by
  // the host on its main thread). Both are borrowed, not owned.
  GlPresenter(void* window, void* context);
  GlPresenter(const GlPresenter&) = delete;
  GlPresenter& operator=(const GlPresenter&) = delete;
  ~GlPresenter();

  // Makes the context current on the calling thread, loads the GL entry
  // points and builds the program. Must be the first call.
  bool Init();

  // Uploads |frame| (kI420 / kYV12 / kNV12) and presents it. Returns false
  // when the format or geometry is not drawable; the caller reports a submit
  // failure. Honors the frame's colorspace/range by uploading the matching
  // conversion matrix.
  bool Present(const VideoFrame& frame);

  // Destroys GL objects. Safe to call twice; after the sink's render thread
  // has stopped, this runs on the host thread and re-makes the context
  // current first.
  void Shutdown();

 private:
  struct Loader;  // Resolved GL entry points; defined in the .cc only.
  bool BuildProgram();
  // (Re)creates the three textures when the coded size or format moved.
  bool EnsureTextures(const VideoFrame& frame);
  void UploadPlanes(const VideoFrame& frame);
  // Fills uMatrix / uOffset from the frame's declared colorspace. CPU-side,
  // in doubles, for the same reason video_convert.cc selects matrices
  // explicitly: a hard-wired BT.601 kernel is wrong for BT.709 HD sources.
  void UpdateColorUniforms(const VideoFrame& frame);

  void* window_;     // SDL_Window*
  void* context_;    // SDL_GLContext
  bool gl_current_{false};
  std::unique_ptr<Loader> loader_;

  unsigned int program_{0};  // GLuint
  unsigned int vao_{0};
  unsigned int vbo_{0};
  unsigned int textures_[3] = {0, 0, 0};
  int texture_width_{0};
  int texture_height_{0};
  int texture_format_{0};  // VideoFormat as int; 0 = kUnknown, i.e. none.
};

}  // namespace avbase::media

#endif  // AVBASE_PLATFORM_SDL2_GL_PRESENT_H_
