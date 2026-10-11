// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_HWIMPORT_HW_IMPORTER_FACTORY_H_
#define AVBASE_PLATFORM_HWIMPORT_HW_IMPORTER_FACTORY_H_

#include <memory>

#include "media/gl/gl_hw_texture_importer.h"

namespace avbase::platform::hwimport {

// The host's GPU-interop backend, or null when this host has none.
//
// ONE FUNCTION, NOT ONE PER WINDOW SYSTEM. The interop API is an axis of its
// own: CGL/IOSurface, EGLImage and WGL_NV_DX_interop do not care whether the
// window came from SDL2, GLFW or EGL directly, and every GL backend wants the
// same answer. Putting the choice here is what lets each of them ask for "the
// importer" without learning which API their host has, and it is the same
// split render/gl uses for gl::NativeGLContext.
//
// The result holds no GL objects and does not need a context to be current to
// be built, so it is per-SINK rather than per-context: a backend builds one
// next to the sink and hands it over. A null return is not a failure -- it is
// what Linux and Windows report until their backends exist, and the sink turns
// it into the explicit CPU readback that has always been the fallback.
std::unique_ptr<media::GlHwTextureImporter> CreateHwTextureImporter();

}  // namespace avbase::platform::hwimport

#endif  // AVBASE_PLATFORM_HWIMPORT_HW_IMPORTER_FACTORY_H_
