// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_GL_SHADERS_H_
#define AVBASE_RENDER_GL_GL_SHADERS_H_

#include <string>

#include "render/gl/gl_api.h"

namespace avbase::gl {

// The header a shader source needs for this context: the right "#version", and
// on GLSL ES the mandatory precision qualifiers.
//
// This exists because desktop GLSL and GLSL ES are different languages wearing
// the same syntax. "#version 330 core" is a compile error on an ES context,
// "#version 300 es" is one on a desktop context, and a shader that is missing
// a version line altogether defaults to 1.10 -- which then rejects every
// in vec2 / out vec4 in the file with an error that names the wrong problem.
// A backend-neutral module cannot hard-code any of the three.
//
// Usage: the constants below are *bodies*, with no #version line. Concatenate:
//
//   const std::string source = ShaderPrologue(gl) + kQuadVertexShaderBody;
std::string ShaderPrologue(const GlApi& gl);

// The four bodies are pointers rather than `const char kFoo[]` arrays because
// the tree's clang-tidy config bans C arrays (modernize-avoid-c-arrays), and an
// array bound would buy nothing: every call site concatenates one of these into
// a std::string. Declaring them as pointers is also the version-proof spelling
// -- whether an array triggers the check has moved between clang-tidy releases.

// Vertex stage for GlQuad: passes position and texcoord straight through.
extern const char* const kQuadVertexShaderBody;

// Vertex stage for GlQuad that places the quad inside a sub-rectangle instead
// of the full viewport. uRect is (x, y, width, height) in NDC, measured from
// the centre -- i.e. the destination rect the overlay pass composites into.
extern const char* const kQuadVertexShaderWithRectBody;

// Fragment stage: samples uTex with whatever texture unit the caller bound.
extern const char* const kTextureFragmentShaderBody;

// Fragment stage: samples uTex and multiplies by uTint (RGBA), so one program
// covers both plain blits and fades.
extern const char* const kTextureTintFragmentShaderBody;

}  // namespace avbase::gl

#endif  // AVBASE_RENDER_GL_GL_SHADERS_H_
