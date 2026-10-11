// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_GL_FRAMEBUFFER_H_
#define AVBASE_RENDER_GL_GL_FRAMEBUFFER_H_

#include <memory>
#include <string>

#include "render/gl/gl_api.h"
#include "render/gl/gl_export.h"
#include "render/gl/gl_texture.h"

namespace avbase::gl {

// An off-screen RGBA8 render target: one colour texture attached to one
// framebuffer object.
//
// This is the piece that lets drawing happen somewhere other than the window.
// Once a frame lands in a texture rather than straight in the drawable, it can
// be scaled to a different output size, drawn more than once, sampled by a
// later pass, or read back with glReadPixels -- none of which is possible when
// the only destination is the default framebuffer.
//
// The colour texture is a GlTexture rather than a bare GL name, so allocation,
// the sampler parameters every sampled texture needs, and destruction stay in
// the one primitive that already does them. What is left here is what only a
// render target has: the attachment and the completeness check.
//
// ORIENTATION: a framebuffer's texel row 0 is the BOTTOM of the viewport, so a
// pass that draws through GlQuad(flip_vertical = false) -- the orientation an
// ordinary top-down image needs -- leaves this texture with its row 0 holding
// the BOTTOM of the picture. Everything that samples such a texture back out
// (GlBlitter, GlTransition) therefore needs GlQuad(flip_vertical = true).
// Getting this backwards produces a picture that is simply upside down, with
// no error anywhere, so it is asserted in tests/unit/gl/ against read-back
// pixels rather than left to inspection.
//
// Borrows the GlApi; both must be destroyed while the context is current.
class AVBASE_GL_EXPORT GlFramebuffer {
 public:
  static std::unique_ptr<GlFramebuffer> Create(const GlApi& gl);
  GlFramebuffer(const GlFramebuffer&) = delete;
  GlFramebuffer& operator=(const GlFramebuffer&) = delete;
  ~GlFramebuffer();

  // Allocates (or reallocates) the target at |width| x |height|. Idempotent:
  // the same size reuses the existing texture and attachment, which is what
  // keeps a per-frame call free. Returns false for a non-positive size, on
  // object-creation failure, or when the driver reports the combination
  // incomplete -- in which case the target is left unallocated rather than
  // half-built, and |error| says which of the three it was.
  bool Ensure(int width, int height, std::string* error);

  // Binds the framebuffer AND sets the viewport to its size. The viewport is
  // part of the operation on purpose: a render target whose viewport still
  // covers the window is the classic way to draw a correct frame into the
  // wrong quarter of it.
  void Bind();

  // Returns to the default framebuffer. Does not restore the viewport -- the
  // caller is about to set its own.
  void Unbind();

  // 0 until a successful Ensure().
  unsigned int id() const { return fbo_; }
  // The GL name to sample the composed image from; 0 until a successful
  // Ensure(). Not a GlTexture because callers hand it to glBindTexture (or to
  // GlBlitter), not to a texture-owning object that could outlive this one.
  unsigned int color_texture() const;
  int width() const { return width_; }
  int height() const { return height_; }
  bool valid() const { return fbo_ != 0; }

 private:
  explicit GlFramebuffer(const GlApi* gl) : gl_(gl) {}

  void Destroy();

  const GlApi* gl_;
  std::unique_ptr<GlTexture> color_texture_;
  unsigned int fbo_{0};
  int width_{0};
  int height_{0};
};

}  // namespace avbase::gl

#endif  // AVBASE_RENDER_GL_GL_FRAMEBUFFER_H_
