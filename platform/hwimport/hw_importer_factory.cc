// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/hwimport/hw_importer_factory.h"

#if defined(__APPLE__)
#include "platform/hwimport/cgl_iosurface_importer.h"
#endif

namespace avbase::platform::hwimport {

std::unique_ptr<media::GlHwTextureImporter> CreateHwTextureImporter() {
#if defined(__APPLE__)
  return CreateCglIosurfaceImporter();
#else
  // Linux would be EGLImage over a VAAPI dmabuf and Windows
  // WGL_NV_DX_interop over a D3D11 texture. Neither is built, and returning
  // nothing is the honest answer rather than a stub that pretends: the sink
  // reads the frame back on the CPU exactly as it did before, so a hardware
  // frame stays CORRECT on those hosts -- just not free. What it must never do
  // is claim UploadPath::kHwZeroCopy for a copy.
  return nullptr;
#endif
}

}  // namespace avbase::platform::hwimport
