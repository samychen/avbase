// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_GL_BLITTER_H_
#define AVBASE_RENDER_GL_GL_BLITTER_H_

#include <memory>
#include <string>

#include "render/gl/gl_api.h"
#include "render/gl/gl_export.h"
#include "render/gl/gl_program.h"
#include "render/gl/gl_quad.h"

namespace avbase::gl {

// Draws a texture over a whole viewport, scaling it to fit: the "copy this
// texture to that target" step.
//
// It exists because once a frame is composed into a GlFramebuffer there has to
// be a way to get it onto the window, and a shader that samples one texture
// needs a program and geometry to run over. Walking the quad twice is the cost
// of the separation; the alternative -- rendering straight to the drawable --
// is what makes an off-screen composition impossible in the first place.
//
// ORIENTATION -- the one thing that is easy to get backwards here, and the
// reason tests/unit/gl checks it against read-back pixels.
//
// The rendering quad (GlQuad with flip_vertical = false) points NDC y = -1 at
// v = 0, which is what a top-down CPU bitmap needs: its first row is uploaded
// to v = 0, so it lands at the top of the viewport.
//
// A framebuffer is not a top-down bitmap. Its texel row 0 -- v = 0 -- is the
// BOTTOM of the viewport. So a frame composed through the upright quad has its
// image bottom stored at v = 0, and blitting it back needs NDC y = -1 to read
// v = 0 as well: flip_vertical = TRUE. Reusing the upright quad here yields a
// perfectly plausible frame that is upside down.
//
// Borrows the GlApi; both must be destroyed while the context is current.
class AVBASE_GL_EXPORT GlBlitter {
 public:
  // Compiles the program and builds the quad. Returns null on failure and,
  // when |error| is non-null, fills it with the compiler/linker log.
  static std::unique_ptr<GlBlitter> Create(const GlApi& gl, std::string* error);
  GlBlitter(const GlBlitter&) = delete;
  GlBlitter& operator=(const GlBlitter&) = delete;
  ~GlBlitter();

  // Sets the viewport to |dst_width| x |dst_height| and fills it with
  // |texture|. A no-op for a zero texture or a non-positive destination, so a
  // caller mid-teardown does not have to special-case itself.
  void Blit(unsigned int texture, int dst_width, int dst_height) const;

 private:
  explicit GlBlitter(const GlApi* gl) : gl_(gl) {}

  const GlApi* gl_;
  std::unique_ptr<GlProgram> program_;
  std::unique_ptr<GlQuad> quad_;
};

}  // namespace avbase::gl

#endif  // AVBASE_RENDER_GL_GL_BLITTER_H_
