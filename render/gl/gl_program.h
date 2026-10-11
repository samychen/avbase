// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_RENDER_GL_GL_PROGRAM_H_
#define AVBASE_RENDER_GL_GL_PROGRAM_H_

#include <memory>
#include <string>

#include "render/gl/gl_api.h"

namespace avbase::gl {

// A linked vertex+fragment program.
//
// Owns its GL object and deletes it on destruction, so it must be destroyed
// while its context is still current -- the same rule GL itself imposes.
//
// Borrows the GlApi, which must outlive it. There is no logging here on
// purpose: a library that wants to be usable outside avbase cannot call
// avbase's logger, so every failure is reported through an out-parameter and
// the caller decides whether it is a warning or a crash.
class GlProgram {
 public:
  // Compiles and links. Returns null on failure and, when |error| is non-null,
  // fills it with the compiler/linker log (or the failed stage's name).
  static std::unique_ptr<GlProgram> Create(const GlApi& gl,
                                           const char* vertex_source,
                                           const char* fragment_source,
                                           std::string* error);

  GlProgram(const GlProgram&) = delete;
  GlProgram& operator=(const GlProgram&) = delete;
  ~GlProgram();

  void Use() const;

  // Looks the uniform up by name. Returns -1 for a name the linker optimised
  // away, which is not an error: setting a uniform that does not exist is a
  // silent no-op in GL, and callers that set optional uniforms rely on that.
  int UniformLocation(const char* name) const;

  // Uniform setters by name; no-ops when the uniform was optimised out.
  void SetInt(const char* name, int value) const;
  void SetFloat(const char* name, float value) const;
  void SetVec2(const char* name, float x, float y) const;
  void SetVec3(const char* name, float x, float y, float z) const;
  void SetVec4(const char* name, float x, float y, float z, float w) const;
  // Column-major, as GL stores matrices.
  void SetMat3(const char* name, const float* values) const;
  void SetMat4(const char* name, const float* values) const;

  unsigned int id() const { return id_; }
  bool valid() const { return id_ != 0; }

 private:
  explicit GlProgram(const GlApi* gl) : gl_(gl) {}

  // Every setter below has the same two steps: resolve the name, and skip when
  // the linker dropped the uniform. Writing that policy once keeps the "a
  // missing uniform is a silent no-op" rule in a single place -- it used to be
  // restated seven times, once of them differently (SetMat4 alone also tested
  // the entry point for null, which the version gate in GlApi::Load already
  // rules out).
  template <class Apply>
  void WithUniform(const char* name, Apply&& apply) const {
    const int kLocation = UniformLocation(name);
    if (kLocation >= 0) {
      apply(kLocation);
    }
  }

  const GlApi* gl_;
  unsigned int id_{0};
};

}  // namespace avbase::gl

#endif  // AVBASE_RENDER_GL_GL_PROGRAM_H_
