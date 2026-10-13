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

// Concatenate before kQuadVertexShaderWithRectBody to make that stage rotate
// the quad about its own centre before placing it; it then also takes a
// `uniform vec2 uRotation` = (cos, sin) of the turn.
//
// A define rather than a second body, and rather than an unguarded uniform, for
// two reasons. The uniform is the dangerous part: a `vec2` nobody has set reads
// back as (0, 0), and (0, 0) is not the identity -- it sends every vertex to
// the centre, so a program that simply forgot to set it draws a blank frame
// with no error and no log line. Guarding it means the stage has no such
// uniform unless the caller opted in. And a define rather than a second copy of
// the text keeps the two spellings from drifting, which is the same reason the
// sampler type is a define above.
extern const char* const kVertexRotationDefine;

// Concatenate before kQuadVertexShaderWithRectBody to make that stage sample a
// sub-rectangle of the source instead of all of it; it then also takes a
// `uniform vec4 uSrcRect` = (x, y, w, h) in normalised source coordinates, with
// (0,0) the first row and column of the picture as stored.
//
// Same shape as uRect on purpose -- both are (origin, size) boxes -- but in a
// different space: uRect is in the DESTINATION and measured from its centre,
// uSrcRect is in the SOURCE and measured from its corner.
extern const char* const kVertexCropDefine;

// Fragment stage: samples uTex with whatever texture unit the caller bound.
extern const char* const kTextureFragmentShaderBody;

// Fragment stage: samples uTex and multiplies by uTint (RGBA), so one program
// covers both plain blits and fades.
extern const char* const kTextureTintFragmentShaderBody;

// Fragment stage: mixes two textures by uProgress (0 = uFrom, 1 = uTo) with the
// same weight everywhere -- the dissolve.
extern const char* const kCrossFadeFragmentShaderBody;

// Fragment stage: the same two textures mixed by uProgress, but with the weight
// a STEP along uDirection instead of a constant, so a hard edge crosses the
// picture. A separate body rather than a uniform on the cross-fade: the two
// differ in the WEIGHT EXPRESSION, which is the one thing a uniform cannot
// select -- only the numbers feeding it.
extern const char* const kWipeFragmentShaderBody;

}  // namespace avbase::gl

#endif  // AVBASE_RENDER_GL_GL_SHADERS_H_
