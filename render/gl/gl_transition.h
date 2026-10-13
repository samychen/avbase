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

// Which weight curve joins the two sides.
//
// A closed set rather than an open one, because each entry is a compiled
// program the caller pays for only when it uses it -- and because naming them
// is the point: "a wipe" has to mean one thing to the caller and the shader.
//
// Deliberately NOT the 36 transitions of the FFmpeg filter: those are a
// product feature of a video editor, and this is a renderer primitive. What
// belongs here is the set that proves the plumbing generalises -- one weight
// that is constant in space and one that is not -- not a catalogue.
enum class TransitionKind {
  // Every pixel crosses together, whatever its position: the dissolve.
  kCrossFade = 0,
  // A hard edge travels across the picture, and |to| is what it has already
  // crossed. Named for where the edge is HEADED, not for where |to| sits:
  // kWipeRight means the edge moves rightwards, so |to| enters from the left.
  kWipeRight,
  kWipeLeft,
  kWipeDown,
  kWipeUp,
};

// Blends two textures over a whole viewport by a progress value.
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
  // Compiles the program for |kind| and builds the quad. Returns null on
  // failure and, when |error| is non-null, fills it with the compile log.
  static std::unique_ptr<GlTransition>
  Create(const GlApi& gl, TransitionKind kind = TransitionKind::kCrossFade,
         std::string* error = nullptr);

  // The kind this object was built for. Exposed because the program is a
  // function of it, so "which transition will run" has to be readable without
  // inspecting the shader.
  TransitionKind kind() const { return kind_; }

  GlTransition(const GlTransition&) = delete;
  GlTransition& operator=(const GlTransition&) = delete;
  ~GlTransition();

  // Sets the viewport to |dst_width| x |dst_height| and fills it with the mix
  // of |from| and |to| at |progress|. 0 shows |from| alone, 1 shows |to|
  // alone; values outside that range are clamped, since a caller driving the
  // value from a clock will overshoot at the ends.
  //
  // A non-finite |progress| is DEGRADED to 0 rather than rejected. This is the
  // per-frame half of the rule and it is the opposite of what a setter does
  // with a bad number, on purpose: a setter can return false and leave the
  // previous value standing, whereas this call returns void, has no previous
  // value to fall back on, and -- being the pass that fills the target --
  // cannot skip the draw without leaving the target holding whatever it held.
  // A defined picture from an undefined number is the only outcome available
  // here. The rejection half belongs to the setters that can report it.
  //
  // A no-op for a zero texture or a non-positive destination, so a caller that
  // has not composed one side yet -- the first frame of a fade-in -- can call
  // it unconditionally.
  void Draw(unsigned int from, unsigned int to, float progress, int dst_width,
            int dst_height) const;

 private:
  explicit GlTransition(const GlApi* gl, TransitionKind kind)
      : gl_(gl), kind_(kind) {}

  const GlApi* gl_;
  const TransitionKind kind_;
  std::unique_ptr<GlProgram> program_;
  std::unique_ptr<GlQuad> quad_;
};

}  // namespace avbase::gl

#endif  // AVBASE_RENDER_GL_GL_TRANSITION_H_
