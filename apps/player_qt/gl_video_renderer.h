// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The video draw, with no Qt in it: entry points, one GlVideoLayer, the rect
// YUV->RGB program, the quad. Separated from gl_video_item.cc for a reason
// that is not taste -- see below.
//
// WHY THIS IS A SEPARATE TRANSLATION UNIT. On macOS, every Qt OpenGL header
// (<QOpenGLContext>, <QOpenGLFunctions>, <QOpenGLFramebufferObject>) reaches
// <OpenGL/gl3.h> through <QtGui/qopengl.h>, and glad's gl.h declares the same
// types and enumerants. Including both in one TU is not a warning but a wall
// of errors: PFNGLTEXIMAGE2DMULTISAMPLEPROC is typedef'd twice with different
// parameter types, and GL_INVALID_INDEX/GL_TIMEOUT_IGNORED are redefined. The
// two cannot coexist, so the code is split by which of them it needs:
//
//   gl_video_item.cc     Qt side -- QQuickFramebufferObject, the FBO, the item
//   gl_video_renderer.cc this file's implementation -- glad, avbase::gl,
//                        media::gl, and nothing from Qt's GL headers
//
// The seam is deliberately Qt-free: the context arrives as void* (a
// QOpenGLContext*, which platform/qt/qt_gl_context.h only forward-declares), so
// this header drags in no Qt header at all and the split stays enforced by the
// compiler rather than by a comment.

#ifndef AVBASE_APPS_PLAYER_QT_GL_VIDEO_RENDERER_H_
#define AVBASE_APPS_PLAYER_QT_GL_VIDEO_RENDERER_H_

#include <memory>
#include <string>

namespace avbase::gl {
class GlApi;
class GlProgram;
class GlQuad;
}  // namespace avbase::gl

namespace avbase::media {
class GlVideoLayer;
class QtGLContext;
class VideoFrame;
}  // namespace avbase::media

namespace avbase::apps {

// One video surface's worth of GL state: the entry-point table, the YUV
// conversion program, the layer that owns the three plane textures, the quad.
//
// THREADING: every method runs on the thread that has the context current --
// for Qt Quick, the scene-graph render thread. Not thread-safe, and not meant
// to be: a GL context is current on one thread at a time.
class GlVideoRenderer {
 public:
  GlVideoRenderer();
  ~GlVideoRenderer();

  GlVideoRenderer(const GlVideoRenderer&) = delete;
  GlVideoRenderer& operator=(const GlVideoRenderer&) = delete;

  // |qt_context| is a QOpenGLContext* and must already be current on the
  // calling thread. Builds everything that outlives a frame; returns false
  // with |error| filled in, and a renderer that failed to init draws nothing.
  bool Init(void* qt_context, int drawable_width, int drawable_height,
            std::string* error);

  // Uploads |frame|'s planes into this layer's textures, taking whichever
  // route the frame calls for (a CPU copy here; a hardware import if an
  // importer is ever installed). False means "there is nothing to draw" -- a
  // format no route can read -- and the caller decides whether to keep the
  // previous picture on screen or drop it.
  bool Upload(const media::VideoFrame& frame);

  // Draws the last uploaded picture into the currently bound framebuffer,
  // letterboxed inside |width| x |height| and turned by |rotation_degrees|.
  // Does nothing until Upload() has succeeded once, because sampling
  // uninitialised textures is worse than drawing nothing.
  void Paint(int width, int height, int rotation_degrees);

  // True once a frame has been uploaded. The caller uses it to tell "no video
  // yet" from "a video whose black frame happens to be black".
  bool has_picture() const;

  // GL_RENDERER, for the readout QML shows.
  const std::string& renderer_name() const { return renderer_name_; }

 private:
  gl::GlProgram* RectProgram();
  void DrawFitted(int width, int height, int rotation_degrees);

  // Declared in dependency order: every other object borrows the GlApi, so
  // destruction runs bottom-up and the table outlives its users.
  std::unique_ptr<media::QtGLContext> context_;
  std::unique_ptr<gl::GlApi> gl_;
  std::unique_ptr<gl::GlProgram> program_;
  std::unique_ptr<gl::GlProgram> rect_program_;
  std::unique_ptr<media::GlVideoLayer> layer_;
  std::unique_ptr<gl::GlQuad> quad_;

  std::string renderer_name_;
};

}  // namespace avbase::apps

#endif  // AVBASE_APPS_PLAYER_QT_GL_VIDEO_RENDERER_H_
