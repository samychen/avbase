// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_GL_TEXTURE_H_
#define AVBASE_RENDER_GL_GL_TEXTURE_H_

#include <memory>

#include "render/gl/gl_api.h"

namespace avbase::gl {

// A texture, on one GL target.
//
// Deliberately thin: it owns the GL name, the target, the sampler parameters
// and the upload calls, and knows nothing about what the pixels mean. Format
// selection, plane splitting and colour conversion belong to the caller --
// that is the boundary that keeps this module out of the video domain.
//
// WHY THE TARGET IS A MEMBER. GL_TEXTURE_2D is what almost every caller wants,
// so it is the default and no existing call site changed. But a texture's
// target is part of its identity -- a GL_TEXTURE_RECTANGLE object cannot be
// bound, sampled or parameterised through the 2D target, and it is sampled
// with texel coordinates rather than normalised ones -- so it has to travel
// with the name. The one caller that needs it is the macOS IOSurface import
// path, whose only working target is GL_TEXTURE_RECTANGLE (see
// platform/hwimport); keeping the target here rather than letting that caller
// call the context directly is what stops a second, half-general texture
// wrapper from growing in the video layer.
//
// Borrows the GlApi; both must be destroyed while the context is current.
class GlTexture {
 public:
  // A GL_TEXTURE_2D texture, the default and what every caller but the
  // rectangle-target import wants.
  static std::unique_ptr<GlTexture> Create(const GlApi& gl);
  static std::unique_ptr<GlTexture> Create(const GlApi& gl,
                                           unsigned int target);
  GlTexture(const GlTexture&) = delete;
  GlTexture& operator=(const GlTexture&) = delete;
  ~GlTexture();

  // Binds to an explicit unit (0..max_texture_units-1).
  void Bind(unsigned int unit) const;
  // Binds to whatever unit is already active.
  void Bind() const;

  // Allocates storage. |data| may be null, which is the usual case when the
  // pixels arrive later through SetSubImage2D per frame.
  //
  // An import path has no use for this: it replaces level 0 through its own
  // API, and allocating first would only throw the allocation away.
  void SetImage2D(unsigned int internal_format, int width, int height,
                  unsigned int format, unsigned int type,
                  const void* data) const;
  void SetSubImage2D(int x, int y, int width, int height, unsigned int format,
                     unsigned int type, const void* data) const;

  // Sampler parameters, individually and as the two combinations that cover
  // most 2D use.
  void SetFilter(unsigned int min_filter, unsigned int mag_filter) const;
  void SetWrap(unsigned int wrap_s, unsigned int wrap_t) const;
  void SetLinearClamp() const;
  void SetNearestClamp() const;

  // EXT_texture_filter_anisotropic. No-op when unsupported, so callers can ask
  // unconditionally.
  void SetAnisotropy(float samples) const;

  // EXT_texture_sRGB_decode: turns the hardware sRGB->linear decode on or off
  // for this texture. No-op when unsupported. |decode| = true means "decode",
  // i.e. the sampled values are linear; false keeps the raw encoded values,
  // which is what a YUV video texture wants.
  void SetSrgbDecode(bool decode) const;

  // GL_UNPACK_ROW_LENGTH: lets a sub-image upload read a larger stride than
  // width, so a cropped plane does not need a repacking pass on the CPU.
  // Context state, not texture state, despite living here.
  void SetUnpackRowLength(int pixels) const;

  void GenerateMipmap() const;

  unsigned int id() const { return id_; }
  unsigned int target() const { return target_; }
  bool valid() const { return id_ != 0; }

 private:
  explicit GlTexture(const GlApi* gl, unsigned int target)
      : gl_(gl), target_(target) {}

  const GlApi* gl_;
  unsigned int id_{0};
  unsigned int target_{0};
};

}  // namespace avbase::gl

#endif  // AVBASE_RENDER_GL_GL_TEXTURE_H_
