// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/gl/gl_framebuffer.h"

#include <memory>
#include <string>

namespace avbase::gl {
namespace {

// Why the target was left unallocated. Kept as three distinct sentences
// because they call for three different reactions: a bad size is a caller
// bug, a missing name is a driver/resource problem, and an incomplete
// framebuffer is a format/driver combination the caller has to give up on.
constexpr char kBadSize[] = "framebuffer: width and height must be positive";
constexpr char kNoObjects[] = "framebuffer: object creation failed";
constexpr char kIncomplete[] = "framebuffer: incomplete at requested size";

// Reports a failure through the out-parameter convention this module uses
// (no logging here -- a module meant to be taken on its own cannot call
// avbase's logger) and returns false, so every failure path is one statement.
bool Fail(std::string* error, const char* message) {
  if (error) {
    *error = message;
  }
  return false;
}

}  // namespace

std::unique_ptr<GlFramebuffer> GlFramebuffer::Create(const GlApi& gl) {
  return std::unique_ptr<GlFramebuffer>(new GlFramebuffer(&gl));
}

GlFramebuffer::~GlFramebuffer() {
  Destroy();
}

unsigned int GlFramebuffer::color_texture() const {
  return color_texture_ ? color_texture_->id() : 0;
}

bool GlFramebuffer::Ensure(int width, int height, std::string* error) {
  if (width <= 0 || height <= 0) {
    return Fail(error, kBadSize);
  }
  // The idempotence that makes this callable every frame: same size, nothing
  // to do. Deliberately keyed on the requested size rather than on fbo_ being
  // non-zero, so a reallocation cannot slip through as a no-op.
  if (fbo_ != 0 && width_ == width && height_ == height) {
    return true;
  }
  Destroy();

  color_texture_ = GlTexture::Create(*gl_);
  if (!color_texture_ || !color_texture_->valid()) {
    Destroy();
    return Fail(error, kNoObjects);
  }
  color_texture_->SetImage2D(GL_RGBA8, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                             nullptr);
  // The parameters a sampled texture needs; without them the blit would read
  // it as mipmap-incomplete and sample black.
  color_texture_->SetLinearClamp();

  gl_->context().GenFramebuffers(1, &fbo_);
  if (fbo_ == 0) {
    Destroy();
    return Fail(error, kNoObjects);
  }
  gl_->context().BindFramebuffer(GL_FRAMEBUFFER, fbo_);
  gl_->context().FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      GL_TEXTURE_2D, color_texture_->id(), 0);
  const GLenum status = gl_->context().CheckFramebufferStatus(GL_FRAMEBUFFER);
  // Unbind before deciding, so a rejected target is not left bound.
  gl_->context().BindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    Destroy();
    return Fail(error, kIncomplete);
  }

  width_ = width;
  height_ = height;
  return true;
}

void GlFramebuffer::Destroy() {
  // The framebuffer goes first: it holds a reference to the texture, and
  // deleting the attachment out from under a live framebuffer is the kind of
  // ordering the driver only complains about later.
  if (fbo_ != 0) {
    gl_->context().DeleteFramebuffers(1, &fbo_);
    fbo_ = 0;
  }
  color_texture_.reset();
  width_ = 0;
  height_ = 0;
}

void GlFramebuffer::Bind() {
  gl_->context().BindFramebuffer(GL_FRAMEBUFFER, fbo_);
  gl_->context().Viewport(0, 0, width_, height_);
}

void GlFramebuffer::Unbind() {
  gl_->context().BindFramebuffer(GL_FRAMEBUFFER, 0);
}

}  // namespace avbase::gl
