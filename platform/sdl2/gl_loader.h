// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_SDL2_GL_LOADER_H_
#define AVBASE_PLATFORM_SDL2_GL_LOADER_H_

#include <SDL.h>

#include <string>

// GL 3.3 core entry points resolved through SDL_GL_GetProcAddress. No GLEW:
// the types are aliased locally and the constants are plain integers, so no
// system GL header (whose version coverage differs per platform) enters the
// build. Lives beside gl_present.cc, its only consumer.

namespace avbase::media {

using GLuint = unsigned int;
using GLenum = unsigned int;
using GLint = int;
using GLsizei = int;
using GLfloat = float;
using GLboolean = unsigned char;
using GLsizeiptr = long;

constexpr GLenum kGLTexture2D = 0x0DE1;
constexpr GLenum kGLRGBA = 0x1908;
constexpr GLenum kGLRGBA8 = 0x8058;
constexpr GLenum kGLBlend = 0x0BE2;
constexpr GLenum kGLSrcAlpha = 0x0302;
constexpr GLenum kGLOneMinusSrcAlpha = 0x0303;
constexpr GLenum kGLRed = 0x1903;
constexpr GLenum kGLRG = 0x8227;
constexpr GLenum kGLR8 = 0x8229;
constexpr GLenum kGLRG8 = 0x822A;
constexpr GLenum kGLUnsignedByte = 0x1401;
constexpr GLenum kGLFloat = 0x1406;
constexpr GLenum kGLLinear = 0x2601;
constexpr GLenum kGLClampToEdge = 0x812F;
constexpr GLenum kGLTextureWrapS = 0x2802;
constexpr GLenum kGLTextureWrapT = 0x2803;
constexpr GLenum kGLTextureMinFilter = 0x2801;
constexpr GLenum kGLTextureMagFilter = 0x2800;
constexpr GLenum kGLUnpackAlignment = 0x0CF5;
constexpr GLenum kGLArrayBuffer = 0x8892;
constexpr GLenum kGLStaticDraw = 0x88E4;
constexpr GLenum kGLTriangleStrip = 0x0005;
constexpr GLenum kGLColorBufferBit = 0x00004000;
constexpr GLenum kGLActiveTexture0 = 0x84C0;
constexpr GLenum kGLActiveTexture1 = 0x84C1;
constexpr GLenum kGLActiveTexture2 = 0x84C2;
constexpr GLenum kGLFragmentShader = 0x8B30;
constexpr GLenum kGLVertexShader = 0x8B33;
constexpr GLenum kGLCompileStatus = 0x8B81;
constexpr GLenum kGLLinkStatus = 0x8B82;

// GLAPIENTRY is not defined without GL headers; every entry point here uses
// the platform's standard calling convention.
#ifndef GLAPIENTRY
#define GLAPIENTRY
#endif

// One entry point per row: (name, return type, parameter list); resolved
// once in Init() via SDL_GL_GetProcAddress.
#define AVBASE_GL_FOREACH(F)                                                   \
  F(CreateShader, GLuint, (GLenum))                                            \
  F(ShaderSource, void, (GLuint, GLsizei, const char* const*, const GLint*))   \
  F(CompileShader, void, (GLuint))                                             \
  F(GetShaderiv, void, (GLuint, GLenum, GLint*))                               \
  F(GetShaderInfoLog, void, (GLuint, GLsizei, GLsizei*, char*))                \
  F(CreateProgram, GLuint, (void))                                             \
  F(AttachShader, void, (GLuint, GLuint))                                      \
  F(LinkProgram, void, (GLuint))                                               \
  F(GetProgramiv, void, (GLuint, GLenum, GLint*))                              \
  F(GetProgramInfoLog, void, (GLuint, GLsizei, GLsizei*, char*))               \
  F(UseProgram, void, (GLuint))                                                \
  F(GetUniformLocation, GLint, (GLuint, const char*))                          \
  F(Uniform1i, void, (GLint, GLint))                                           \
  F(Uniform3f, void, (GLint, GLfloat, GLfloat, GLfloat))                       \
  F(Uniform4f, void, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))              \
  F(Enable, void, (GLenum))                                                    \
  F(Disable, void, (GLenum))                                                   \
  F(BlendFunc, void, (GLenum, GLenum))                                         \
  F(UniformMatrix3fv, void, (GLint, GLsizei, GLboolean, const GLfloat*))       \
  F(DeleteShader, void, (GLuint))                                              \
  F(DeleteProgram, void, (GLuint))                                             \
  F(GenVertexArrays, void, (GLsizei, GLuint*))                                 \
  F(BindVertexArray, void, (GLuint))                                           \
  F(DeleteVertexArrays, void, (GLsizei, const GLuint*))                        \
  F(GenBuffers, void, (GLsizei, GLuint*))                                      \
  F(BindBuffer, void, (GLenum, GLuint))                                        \
  F(BufferData, void, (GLenum, GLsizeiptr, const void*, GLenum))               \
  F(DeleteBuffers, void, (GLsizei, const GLuint*))                             \
  F(EnableVertexAttribArray, void, (GLuint))                                   \
  F(VertexAttribPointer, void,                                                 \
    (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))                  \
  F(GenTextures, void, (GLsizei, GLuint*))                                     \
  F(BindTexture, void, (GLenum, GLuint))                                       \
  F(DeleteTextures, void, (GLsizei, const GLuint*))                            \
  F(TexImage2D, void,                                                          \
    (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,            \
     const void*))                                                             \
  F(TexSubImage2D, void,                                                       \
    (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,            \
     const void*))                                                             \
  F(TexParameteri, void, (GLenum, GLenum, GLint))                              \
  F(PixelStorei, void, (GLenum, GLint))                                        \
  F(ActiveTexture, void, (GLenum))                                             \
  F(Viewport, void, (GLint, GLint, GLsizei, GLsizei))                          \
  F(ClearColor, void, (GLfloat, GLfloat, GLfloat, GLfloat))                    \
  F(Clear, void, (GLenum))                                                     \
  F(DrawArrays, void, (GLenum, GLint, GLsizei))


// Resolves every entry point; returns false and names the first missing one
// when the runtime is too old (e.g. a GL 2.0-era driver). Must be called with
// the context current on the calling thread.
// One member per entry point; ResolveGl() fills them.
struct GlLoader {
#define AVBASE_GL_MEMBER(name, ret, params) \
  ret(GLAPIENTRY* name) params = nullptr;
  AVBASE_GL_FOREACH(AVBASE_GL_MEMBER)
#undef AVBASE_GL_MEMBER
};

inline bool ResolveGl(GlLoader* gl, std::string* missing) {
#define AVBASE_GL_RESOLVE(name, ret, params)                              \
  gl->name =                                                              \
      reinterpret_cast<decltype(gl->name)>(SDL_GL_GetProcAddress("gl" #name)); \
  if (!gl->name) {                                                        \
    if (missing) {                                                        \
      *missing = #name;                                                   \
    }                                                                     \
    return false;                                                         \
  }
  AVBASE_GL_FOREACH(AVBASE_GL_RESOLVE)
#undef AVBASE_GL_RESOLVE
  return true;
}

}  // namespace avbase::media

#endif  // AVBASE_PLATFORM_SDL2_GL_LOADER_H_
