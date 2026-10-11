// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/gl/gl_transition.h"

#include <algorithm>
#include <string>

#include "render/gl/gl_shaders.h"

namespace avbase::gl {
namespace {

// Two samplers, two units, named once because each has to agree between the
// uniform it feeds and the ActiveTexture call that binds it.
constexpr int kFromUnit = 0;
constexpr int kToUnit = 1;

}  // namespace

std::unique_ptr<GlTransition> GlTransition::Create(const GlApi& gl,
                                                   std::string* error) {
  std::unique_ptr<GlTransition> transition(new GlTransition(&gl));

  const std::string prologue = ShaderPrologue(gl);
  transition->program_ = GlProgram::Create(
      gl, (prologue + kQuadVertexShaderBody).c_str(),
      (prologue + kCrossFadeFragmentShaderBody).c_str(), error);
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
