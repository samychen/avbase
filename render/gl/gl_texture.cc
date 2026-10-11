// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/gl/gl_texture.h"

namespace avbase::gl {

std::unique_ptr<GlTexture> GlTexture::Create(const GlApi& gl) {
  GLuint name = 0;
  gl.context().GenTextures(1, &name);
  if (name == 0) {
    return nullptr;
  }
  auto texture = std::unique_ptr<GlTexture>(new GlTexture(&gl));
  texture->id_ = name;
  return texture;
}

GlTexture::~GlTexture() {
  if (id_ != 0) {
    gl_->context().DeleteTextures(1, &id_);
    id_ = 0;
  }
}

void GlTexture::Bind(unsigned int unit) const {
  gl_->context().ActiveTexture(GL_TEXTURE0 + unit);
  gl_->context().BindTexture(GL_TEXTURE_2D, id_);
}

void GlTexture::Bind() const {
  gl_->context().BindTexture(GL_TEXTURE_2D, id_);
}

void GlTexture::SetImage2D(unsigned int internal_format, int width, int height,
                           unsigned int format, unsigned int type,
                           const void* data) const {
  gl_->context().BindTexture(GL_TEXTURE_2D, id_);
  gl_->context().TexImage2D(GL_TEXTURE_2D, 0, static_cast<int>(internal_format),
                            width, height, 0, format, type, data);
}

void GlTexture::SetSubImage2D(int x, int y, int width, int height,
                              unsigned int format, unsigned int type,
                              const void* data) const {
  gl_->context().BindTexture(GL_TEXTURE_2D, id_);
  gl_->context().TexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, format,
                               type, data);
}

void GlTexture::SetFilter(unsigned int min_filter,
                          unsigned int mag_filter) const {
  gl_->context().BindTexture(GL_TEXTURE_2D, id_);
  gl_->context().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                               static_cast<int>(min_filter));
  gl_->context().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                               static_cast<int>(mag_filter));
}

void GlTexture::SetWrap(unsigned int wrap_s, unsigned int wrap_t) const {
  gl_->context().BindTexture(GL_TEXTURE_2D, id_);
  gl_->context().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                               static_cast<int>(wrap_s));
  gl_->context().TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                               static_cast<int>(wrap_t));
}

void GlTexture::SetLinearClamp() const {
  SetFilter(GL_LINEAR, GL_LINEAR);
  SetWrap(GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
}

void GlTexture::SetNearestClamp() const {
  SetFilter(GL_NEAREST, GL_NEAREST);
  SetWrap(GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
}

void GlTexture::SetAnisotropy(float samples) const {
  if (!gl_->has_anisotropic_filter()) {
    return;
  }
  gl_->context().BindTexture(GL_TEXTURE_2D, id_);
  gl_->context().TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                               samples);
}

void GlTexture::SetSrgbDecode(bool decode) const {
  if (!gl_->has_srgb_decode()) {
    return;
  }
  gl_->context().BindTexture(GL_TEXTURE_2D, id_);
  gl_->context().TexParameteri(
      GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT,
      static_cast<int>(decode ? GL_DECODE_EXT : GL_SKIP_DECODE_EXT));
}

void GlTexture::SetUnpackRowLength(int pixels) const {
  if (pixels <= 0) {
    gl_->context().PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    return;
  }
  gl_->context().PixelStorei(GL_UNPACK_ROW_LENGTH, pixels);
}

void GlTexture::GenerateMipmap() const {
  if (gl_->context().GenerateMipmap != nullptr) {
    gl_->context().BindTexture(GL_TEXTURE_2D, id_);
    gl_->context().GenerateMipmap(GL_TEXTURE_2D);
  }
}

}  // namespace avbase::gl
