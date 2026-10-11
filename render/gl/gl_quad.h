// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_GL_QUAD_H_
#define AVBASE_RENDER_GL_GL_QUAD_H_

#include <memory>

#include "render/gl/gl_api.h"

namespace avbase::gl {

// A unit quad covering NDC [-1, 1]^2, drawn as a four-vertex triangle strip.
//
// It fixes the vertex-attribute convention the shaders in this module assume:
//
//   location 0  vec2  position   NDC
//   location 1  vec2  texcoord   0..1
//
// Any shader pair that declares those two inputs can be paired with it. The
// position is written to a VBO rather than derived from gl_VertexID so the
// same quad also works on a GLES2-era context, where gl_VertexID does not
// exist.
//
// Borrows the GlApi; both must be destroyed while the context is current.
class GlQuad {
 public:
  // |flip_vertical| reverses the texcoord V axis. GL puts texcoord V=0 at the
  // first row of the texture data and NDC y=-1 at the bottom of the viewport,
  // so "false" presents a conventional top-down image upright. Set it to true
  // when the source's row order is already bottom-up.
  static std::unique_ptr<GlQuad> Create(const GlApi& gl,
                                        bool flip_vertical = false);
  GlQuad(const GlQuad&) = delete;
  GlQuad& operator=(const GlQuad&) = delete;
  ~GlQuad();

  // Binds the VAO and draws. The caller has already selected the program and
  // bound its textures; this only issues the geometry.
  void Draw() const;

  unsigned int vao() const { return vao_; }

 private:
  explicit GlQuad(const GlApi* gl) : gl_(gl) {}

  const GlApi* gl_;
  unsigned int vao_{0};
  unsigned int vbo_{0};
};

}  // namespace avbase::gl

#endif  // AVBASE_RENDER_GL_GL_QUAD_H_
