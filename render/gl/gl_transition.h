// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_GL_TRANSITION_H_
#define AVBASE_RENDER_GL_GL_TRANSITION_H_

#include <memory>
#include <string>

#include "render/gl/gl_api.h"
#include "render/gl/gl_export.h"
#include "render/gl/gl_program.h"
#include "render/gl/gl_quad.h"

namespace avbase::gl {

// Blends two textures over a whole viewport by a progress value: the
// cross-fade step, and the only transition the module ships.
//
// A transition is a compositor feature, not a video one: it takes two finished
// images and produces a third. That is why it lives here next to GlBlitter
// rather than with the scene: a wipe, a slide or a dissolve differ only in the
// weight shader, and the plumbing they share -- two sampler units, the
// geometry, the orientation, the destination sizing -- is this class.
//
// WHY PROGRESS IS A FLOAT AND NOT A TIME: this class does not own a clock and
// does not know what is being transitioned. A caller that wants a one-second
// fade computes the fraction and passes it; the same call then serves a
// keyframed cut, a scrub on a timeline, or a fixed 0.5 for a still.
//
// ORIENTATION: both sources must be FRAMEBUFFER textures -- a GlFramebuffer's
// colour attachment, or anything else whose row 0 is the bottom of its
// viewport -- and the pass therefore uses GlQuad(flip_vertical = true), the
// same as GlBlitter.
//
// The reason is worth stating because the obvious guess is the opposite one.
// Drawing an ordinary top-down image through the upright quad puts the image's
// first row at the TOP of the viewport, which is the LAST row of the
// framebuffer -- so a framebuffer texture's row 0 holds the BOTTOM of its
// picture. "Row 0 of the source" and "row 0 of a top-down reader" therefore
// name opposite ends, and the sampled copy has to be flipped to compensate.
// A texture uploaded from a CPU bitmap is NOT framebuffer-shaped: passing one
// here produces a picture that is simply upside down.
//
// tests/unit/gl pins this against read-back pixels. Getting it wrong is
// invisible in every other respect: every call succeeds, every GL error is
// GL_NO_ERROR, and only the picture is wrong.
//
// Borrows the GlApi; both must be destroyed while the context is current.
class AVBASE_GL_EXPORT GlTransition {
 public:
  // Compiles the cross-fade program and builds the quad. Returns null on
  // failure and, when |error| is non-null, fills it with the compile log.
  static std::unique_ptr<GlTransition> Create(const GlApi& gl,
                                              std::string* error);
  GlTransition(const GlTransition&) = delete;
  GlTransition& operator=(const GlTransition&) = delete;
  ~GlTransition();

  // Sets the viewport to |dst_width| x |dst_height| and fills it with the mix
  // of |from| and |to| at |progress|. 0 shows |from| alone, 1 shows |to|
  // alone; values outside that range are clamped, since a caller driving the
  // value from a clock will overshoot at the ends.
  //
  // A no-op for a zero texture or a non-positive destination, so a caller that
  // has not composed one side yet -- the first frame of a fade-in -- can call
  // it unconditionally.
  void Draw(unsigned int from, unsigned int to, float progress, int dst_width,
            int dst_height) const;

 private:
  explicit GlTransition(const GlApi* gl) : gl_(gl) {}

  const GlApi* gl_;
  std::unique_ptr<GlProgram> program_;
  std::unique_ptr<GlQuad> quad_;
};

}  // namespace avbase::gl

#endif  // AVBASE_RENDER_GL_GL_TRANSITION_H_
