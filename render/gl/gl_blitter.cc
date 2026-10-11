// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/gl/gl_blitter.h"

#include <string>

#include "render/gl/gl_shaders.h"

namespace avbase::gl {
namespace {

// The blitter samples exactly one texture, so its unit is fixed rather than
// parameterised -- but it is still named once, because it has to agree between
// the sampler's uniform and the ActiveTexture call that feeds it.
constexpr int kTextureUnit = 0;

}  // namespace

std::unique_ptr<GlBlitter> GlBlitter::Create(const GlApi& gl,
                                             std::string* error) {
  std::unique_ptr<GlBlitter> blitter(new GlBlitter(&gl));

  const std::string prologue = ShaderPrologue(gl);
  blitter->program_ =
      GlProgram::Create(gl, (prologue + kQuadVertexShaderBody).c_str(),
                        (prologue + kTextureFragmentShaderBody).c_str(), error);
  if (!blitter->program_) {
    return nullptr;
  }
  // The one place the flip is decided; see the header for why it is true.
  blitter->quad_ = GlQuad::Create(gl, /*flip_vertical=*/true);
  if (!blitter->quad_) {
    if (error) {
      *error = "blitter: quad creation failed";
    }
    return nullptr;
  }
  return blitter;
}

GlBlitter::~GlBlitter() = default;

void GlBlitter::Blit(unsigned int texture, int dst_width,
                     int dst_height) const {
  if (texture == 0 || dst_width <= 0 || dst_height <= 0) {
    return;
  }
  gl_->context().Viewport(0, 0, dst_width, dst_height);
  program_->Use();
  program_->SetInt("uTex", kTextureUnit);
  gl_->context().ActiveTexture(GL_TEXTURE0 + kTextureUnit);
  gl_->context().BindTexture(GL_TEXTURE_2D, texture);
  quad_->Draw();
  gl_->context().BindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace avbase::gl
