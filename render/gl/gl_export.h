// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_GL_EXPORT_H_
#define AVBASE_RENDER_GL_GL_EXPORT_H_

// Symbol visibility for the avbase_gl module.
//
// The whole project builds with `CMAKE_CXX_VISIBILITY_PRESET hidden`, so a
// class a consumer outside this module must be able to name, derive from or
// call needs the default visibility explicitly. Only the module's own sources
// define AVBASE_GL_IMPLEMENTATION (see render/CMakeLists.txt), which keeps the
// annotation meaningless-but-harmless everywhere else.
#if defined(AVBASE_GL_IMPLEMENTATION)
#define AVBASE_GL_EXPORT __attribute__((visibility("default")))
#else
#define AVBASE_GL_EXPORT
#endif

#endif  // AVBASE_RENDER_GL_GL_EXPORT_H_
