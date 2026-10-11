// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_HWIMPORT_CGL_IOSURFACE_IMPORTER_H_
#define AVBASE_PLATFORM_HWIMPORT_CGL_IOSURFACE_IMPORTER_H_

#include <memory>

#include "media/gl/gl_hw_texture_importer.h"

namespace avbase::platform::hwimport {

#if defined(__APPLE__)
// macOS: a CVPixelBuffer is IOSurface-backed, and CGLTexImageIOSurface2D
// points a GL texture straight at one of that surface's planes. Null only if
// allocation fails.
//
// The target it can use is GL_TEXTURE_RECTANGLE and NOT GL_TEXTURE_2D, which
// is measured rather than assumed: CGLTexImageIOSurface2D answers kCGLBadValue
// for every 2D combination tried (GL_R8/GL_RED, GL_RG8/GL_RG, GL_LUMINANCE8,
// GL_LUMINANCE) while GL_TEXTURE_RECTANGLE + GL_R8/GL_RED succeeds and samples
// back byte-exact, in a 3.3 CORE profile context, on Intel/Iris under macOS
// 15. Apple's own header agrees -- "target must currently be
// GL_TEXTURE_RECTANGLE_ARB" -- and the consequence is that the video shader
// needs a sampler2DRect variant (kYuvSamplerRectDefines).
std::unique_ptr<media::GlHwTextureImporter> CreateCglIosurfaceImporter();
#endif

}  // namespace avbase::platform::hwimport

#endif  // AVBASE_PLATFORM_HWIMPORT_CGL_IOSURFACE_IMPORTER_H_
