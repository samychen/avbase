// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/hwimport/cgl_iosurface_importer.h"

// Before the OpenGL headers, which is the only place that decides whether the
// whole legacy framework's deprecation attributes expand to anything. GL is
// not deprecated here by accident -- it is the graphics API this tree's GL
// module targets, and the build is -Werror, so without this every CGL call
// would be a build failure rather than a design decision.
#define GL_SILENCE_DEPRECATION 1

#include <CoreVideo/CoreVideo.h>
// gl3.h and not gl.h: this file needs GL_TEXTURE_RECTANGLE, which Apple's
// legacy gl.h does not define (only the _ARB and _EXT spellings in glext.h).
// gl3.h is the SDK's core-profile header and carries the unsuffixed name, so
// the enumerant is spelled by the SDK rather than by hand -- a GL constant
// written out as a literal is this tree's most expensive known trap, because
// getting it wrong does not fail to compile, it fails to draw.
//
// The SYSTEM header is used here and glad is not, because this file talks to
// CGL -- a system API -- and including both worlds' GL headers in one
// translation unit conflicts on the GL types. The caller (media/gl) uses glad
// and hands the rest of the enumerants in as plain integers, which is why the
// one constant that has to agree is exposed as GlHwTextureImporter::target()
// instead of being spelled on both sides.
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <OpenGL/gltypes.h>

#include "base/logging.h"

namespace avbase::platform::hwimport {
namespace {

// Every failure below is reported once and then stays quiet: a driver that
// refuses this API refuses it for every frame, and a log line per frame is a
// log nobody reads. The sink's own fallback (one readback, then a sticky
// disable) is what turns the first refusal into a decision.
void LogOnce(const char* reason) {
  static bool logged = false;
  if (!logged) {
    logged = true;
    LOG(ERROR) << "cgl-iosurface import unavailable: " << reason;
  }
}

CVPixelBufferRef PixelBufferOf(const media::VideoFrame& frame) {
  if (frame.native_handle().kind != media::NativeHandleKind::kCVPixelBuffer) {
    return nullptr;
  }
  // const_cast rather than a non-const handle: the frame's pixels are
  // immutable once published, and this only reads the buffer's identity. The
  // pointer is owned by the frame's producer-side backing and is valid for as
  // long as the frame is (see VideoFrame::WrapNativeBuffer).
  return static_cast<CVPixelBufferRef>(
      const_cast<void*>(frame.native_handle().id));
}

class CglIosurfaceImporter final : public media::GlHwTextureImporter {
 public:
  unsigned int target() const override;
  bool CanImport(const media::VideoFrame& frame) const override;
  bool ImportPlane(const media::VideoFrame& frame, int index,
                   unsigned int internal_format, unsigned int format,
                   unsigned int type, int width, int height) override;
  const char* name() const override { return "cgl-iosurface"; }
};

unsigned int CglIosurfaceImporter::target() const {
  // Not a preference. CGLTexImageIOSurface2D answers kCGLBadValue for every
  // 2D combination (GL_R8/GL_RED, GL_RG8/GL_RG, GL_LUMINANCE8, GL_LUMINANCE)
  // while this one succeeds and samples back byte-exact -- measured in a 3.3
  // CORE profile on Intel/Iris under macOS 15, which is also the combination
  // the GL unit tests use. Apple's header says as much ("target must
  // currently be GL_TEXTURE_RECTANGLE_ARB").
  return GL_TEXTURE_RECTANGLE;
}

bool CglIosurfaceImporter::CanImport(const media::VideoFrame& frame) const {
  // The 2-plane 8-bit bi-planar layout, which is what VideoToolbox produces
  // for H.264 and HEVC and the only one this backend can place: the shader
  // reads one luma plane and one interleaved chroma plane. A 10-bit P010
  // surface is a different upload -- 16-bit elements, and a different matrix
  // range -- and is declined here rather than sampled as if it were 8-bit.
  if (frame.format() != media::VideoFormat::kNV12) {
    return false;
  }
  CVPixelBufferRef pb = PixelBufferOf(frame);
  if (pb == nullptr) {
    return false;
  }
  // Not every CVPixelBuffer is IOSurface-backed; one that is not has no plane
  // for CGL to bind.
  if (CVPixelBufferGetIOSurface(pb) == nullptr) {
    return false;
  }
  return CVPixelBufferGetPlaneCount(pb) == 2;
  // DELIBERATELY NO CONTEXT CHECK HERE. The sink asks this before it has made
  // the context current -- on the first frame the presenter does not exist yet
  // -- so an answer of "no current CGL context" would switch the import off
  // for the sink's whole life on the strength of a question asked at the wrong
  // moment. ImportPlane() is where the context must be current, and it is the
  // one that checks.
}

bool CglIosurfaceImporter::ImportPlane(const media::VideoFrame& frame,
                                       int index, unsigned int internal_format,
                                       unsigned int format, unsigned int type,
                                       int width, int height) {
  // Two planes is all an NV12 surface has, and index is the shader's slot
  // order rather than the surface's: slot 1 is the interleaved chroma.
  if (index < 0 || index > 1 || width <= 0 || height <= 0) {
    return false;
  }
  CVPixelBufferRef pb = PixelBufferOf(frame);
  IOSurfaceRef surface = pb ? CVPixelBufferGetIOSurface(pb) : nullptr;
  if (surface == nullptr) {
    LogOnce("frame is not an IOSurface-backed pixel buffer");
    return false;
  }
  CGLContextObj ctx = CGLGetCurrentContext();
  if (ctx == nullptr) {
    // SDL2, GLFW and EGL all end up with a CGL context on macOS, so this means
    // the caller is not on the thread that made one current -- a bug in the
    // caller, not a limitation of the host.
    LogOnce("no current CGL context on this thread");
    return false;
  }
  const CGLError err = CGLTexImageIOSurface2D(
      ctx, GL_TEXTURE_RECTANGLE, static_cast<GLenum>(internal_format), width,
      height, static_cast<GLenum>(format), static_cast<GLenum>(type), surface,
      static_cast<GLuint>(index));
  if (err != kCGLNoError) {
    LogOnce(CGLErrorString(err));
    return false;
  }
  // A live binding: Apple's header is explicit that the texture keeps reading
  // the surface as it changes, so there is nothing to re-do per frame beyond
  // re-pointing at the frame's own surface -- which is what the next call does.
  return true;
}

}  // namespace

std::unique_ptr<media::GlHwTextureImporter> CreateCglIosurfaceImporter() {
  return std::make_unique<CglIosurfaceImporter>();
}

}  // namespace avbase::platform::hwimport
