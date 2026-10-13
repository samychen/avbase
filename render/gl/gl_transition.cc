// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/gl/gl_transition.h"

#include <algorithm>
#include <array>
#include <string>

#include "render/gl/gl_shaders.h"

namespace avbase::gl {
namespace {

// Two samplers, two units, named once because each has to agree between the
// uniform it feeds and the ActiveTexture call that binds it.
constexpr int kFromUnit = 0;
constexpr int kToUnit = 1;

// Which way a wipe's edge travels, as a unit vector over the picture in SCREEN
// orientation: vTexCoord.y = 0 is the BOTTOM, because this pass samples
// framebuffer textures through the flipped quad (see the header).
//
// A kind is named for where the edge is HEADED, not for where |to| sits, so
// kWipeRight moves the edge rightwards and |to| enters from the LEFT: the
// shader's d is measured along the direction of travel, and what |to| fills is
// what the edge has already crossed.
std::array<float, 2> DirectionOf(TransitionKind kind) {
  switch (kind) {
  case TransitionKind::kWipeRight:
    return {1.0f, 0.0f};
  case TransitionKind::kWipeLeft:
    return {-1.0f, 0.0f};
  case TransitionKind::kWipeDown:
    return {0.0f, -1.0f};
  case TransitionKind::kWipeUp:
    return {0.0f, 1.0f};
  case TransitionKind::kCrossFade:
    break;
  }
  // The cross-fade has no edge to travel; the value is never read.
  return {0.0f, 0.0f};
}

}  // namespace

std::unique_ptr<GlTransition>
GlTransition::Create(const GlApi& gl, TransitionKind kind, std::string* error) {
  std::unique_ptr<GlTransition> transition(new GlTransition(&gl, kind));

  // The one thing a kind changes: the weight expression. Everything the two
  // share -- the samplers, the geometry, the flip -- is below or in Draw().
  const char* weight = kind == TransitionKind::kCrossFade
                           ? kCrossFadeFragmentShaderBody
                           : kWipeFragmentShaderBody;
  const std::string prologue = ShaderPrologue(gl);
  transition->program_ =
      GlProgram::Create(gl, (prologue + kQuadVertexShaderBody).c_str(),
                        (prologue + weight).c_str(), error);
  if (!transition->program_) {
    return nullptr;
  }
  // The one place the flip is decided; true, and see the header for why a
  // framebuffer source needs it.
  transition->quad_ = GlQuad::Create(gl, /*flip_vertical=*/true);
  if (!transition->quad_) {
    if (error) {
      *error = "transition: quad creation failed";
    }
    return nullptr;
  }
  return transition;
}

GlTransition::~GlTransition() = default;

void GlTransition::Draw(unsigned int from, unsigned int to, float progress,
                        int dst_width, int dst_height) const {
  if (from == 0 || to == 0 || dst_width <= 0 || dst_height <= 0) {
    return;
  }
  // std::clamp would pass a NaN straight through and mix() with a NaN weight
  // is undefined, so a non-finite progress is treated as "no progress" rather
  // than propagated into the pixels.
  float t = std::isfinite(progress) ? progress : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);

  gl_->context().Viewport(0, 0, dst_width, dst_height);
  program_->Use();
  program_->SetInt("uFrom", kFromUnit);
  program_->SetInt("uTo", kToUnit);
  program_->SetFloat("uProgress", t);
  // The wipe's travel axis. Set every draw rather than once at Create():
  // uniform state belongs to the PROGRAM, and the cross-fade pass shares the
  // unit plumbing -- so it is either always set here or silently stale.
  // A no-op on the cross-fade program, which declares no such uniform.
  const std::array<float, 2> dir = DirectionOf(kind_);
  program_->SetVec2("uDirection", dir[0], dir[1]);
  gl_->context().ActiveTexture(GL_TEXTURE0 + kFromUnit);
  gl_->context().BindTexture(GL_TEXTURE_2D, from);
  gl_->context().ActiveTexture(GL_TEXTURE0 + kToUnit);
  gl_->context().BindTexture(GL_TEXTURE_2D, to);
  quad_->Draw();
  // Leave the active unit where it started: it is context state, and a caller
  // that binds a texture without an ActiveTexture call would otherwise write
  // into unit 1.
  gl_->context().BindTexture(GL_TEXTURE_2D, 0);
  gl_->context().ActiveTexture(GL_TEXTURE0 + kFromUnit);
  gl_->context().BindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace avbase::gl
