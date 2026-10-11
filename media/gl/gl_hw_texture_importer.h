// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_GL_GL_HW_TEXTURE_IMPORTER_H_
#define AVBASE_MEDIA_GL_GL_HW_TEXTURE_IMPORTER_H_

#include "media/base/video_frame.h"
#include "media/media_export.h"

namespace avbase::media {

// The seam between the GL video path and a host's GPU-interop API.
//
// A hardware-decoded frame never reaches CPU memory -- VideoFrame carries a
// typed NativeHandle for exactly that reason (avbase §6.2). An importer takes
// that handle and points GL textures at the SAME graphics surface the decoder
// wrote, so the pixels go GPU to GPU with no readback and no upload. That is
// the whole of "zero copy", and it is what UploadPath::kHwZeroCopy reports.
//
// WHY THE SHAPE IS HERE AND THE IMPLEMENTATION IS IN platform/. The backends
// are CGL/IOSurface (macOS), EGLImage (Linux) and WGL_NV_DX_interop (Windows);
// each one names a host SDK, and invariant C5 keeps those out of media/. So the
// interface lives here -- media/gl is already shaped by VideoFrame, and names
// no SDK -- and the platform layer supplies an implementation, exactly as
// gl::NativeGLContext is declared in render/gl and implemented by a backend.
// The dependency arrow only ever points platform -> media, which is what keeps
// this from needing a registration hook or a weak symbol.
//
// THREADING: every method runs on the thread that has the GL context current --
// in avbase, the sink's render thread. CanImport() reads only the frame, so any
// thread may ask it while the frame is alive; frames are immutable once
// published, which is what makes that safe.
//
// LIFETIME -- the one rule a caller has to get right. An imported texture
// refers to the frame's graphics surface, and no implementation keeps that
// surface alive: the frame owns it, and its release callback is what frees it.
// So a plane texture may be sampled only between the ImportPlane() call that
// filled it and the next call that replaces it. GlVideoLayer satisfies this by
// importing on every upload, immediately before the draw pass -- the only place
// the plane textures are read -- so no drawn frame ever samples a texture whose
// surface has been released.
class AVBASE_MEDIA_EXPORT GlHwTextureImporter {
 public:
  virtual ~GlHwTextureImporter() = default;

  // The GL texture target this backend binds its planes to.
  //
  // A property of the INTEROP API, not of the frame and not of the caller:
  // CGLTexImageIOSurface2D only accepts GL_TEXTURE_RECTANGLE (measured -- it
  // answers kCGLBadValue for GL_TEXTURE_2D), while an EGLImage import would
  // use GL_TEXTURE_2D. So the backend says which one it needs and the layer
  // creates its textures on it, instead of both sides spelling the same
  // enumerant and hoping the two agree.
  //
  // One definition is worth having because the two sides would otherwise be
  // writing it from different headers -- the caller from glad, the backend
  // from the host's SDK -- and a GL constant that is spelled twice is this
  // tree's most expensive known trap: it does not fail to compile, it fails
  // to draw.
  virtual unsigned int target() const = 0;

  // Whether |frame| can be taken with no CPU copy at all. Takes the frame
  // rather than only the handle kind because the answer depends on the pixel
  // layout too: a 10-bit surface is not the 2-plane 8-bit NV12 upload the
  // shader reads, and the backend is the only thing that knows which formats
  // its interop API accepts.
  virtual bool CanImport(const VideoFrame& frame) const = 0;

  // Binds plane |index| of |frame| to whichever texture is CURRENTLY BOUND to
  // GL_TEXTURE_2D, replacing that texture's storage.
  //
  // The caller binds, and the caller owns the textures, because which texture
  // goes on which unit is the shader's contract and not the interop API's
  // business. For the same reason the caller passes the sampling layout it
  // wants -- |internal_format| / |format| / |type| (GL_R8/GL_RED for luma,
  // GL_RG8/GL_RG for interleaved chroma) -- so no backend has to hard-code a
  // GL constant table, which is this tree's most expensive known trap.
  //
  // |width| and |height| are the plane's own, which for chroma is half the
  // coded size. Returns false when this plane cannot be imported; the caller
  // then holds a partially imported frame and must not draw it.
  virtual bool ImportPlane(const VideoFrame& frame, int index,
                           unsigned int internal_format, unsigned int format,
                           unsigned int type, int width, int height) = 0;

  // For logs and stats: "cgl-iosurface", "egl-image", ...
  virtual const char* name() const = 0;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_HW_TEXTURE_IMPORTER_H_
