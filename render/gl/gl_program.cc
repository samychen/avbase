// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/gl/gl_program.h"

#include <array>
#include <cstddef>

namespace avbase::gl {

std::unique_ptr<GlProgram> GlProgram::Create(const GlApi& gl,
                                             const char* vertex_source,
                                             const char* fragment_source,
                                             std::string* error) {
  if (!vertex_source || !fragment_source) {
    if (error) {
      *error = "null shader source";
    }
    return nullptr;
  }

  const GLuint kVertex = gl.context().CreateShader(GL_VERTEX_SHADER);
  const GLuint kFragment = gl.context().CreateShader(GL_FRAGMENT_SHADER);
  if (kVertex == 0 || kFragment == 0) {
    if (error) {
      *error = "glCreateShader returned 0 (invalid shader type?)";
    }
    return nullptr;
  }

  bool ok = true;
  const std::array<GLuint, 2> kStages = {kVertex, kFragment};
  const std::array<const char*, 2> kSources = {vertex_source, fragment_source};
  for (size_t i = 0; i < kStages.size() && ok; ++i) {
    gl.context().ShaderSource(kStages[i], 1, &kSources[i], nullptr);
    gl.context().CompileShader(kStages[i]);
    GLint status = 0;
    gl.context().GetShaderiv(kStages[i], GL_COMPILE_STATUS, &status);
    if (status != 0) {
      continue;
    }
    ok = false;
    if (error) {
      std::array<char, 512> log = {};
      gl.context().GetShaderInfoLog(
          kStages[i], static_cast<GLsizei>(log.size()), nullptr, log.data());
      *error = std::string(i == 0 ? "vertex" : "fragment") +
               " shader: " + log.data();
    }
  }

  GLuint program = 0;
  if (ok) {
    program = gl.context().CreateProgram();
    gl.context().AttachShader(program, kVertex);
    gl.context().AttachShader(program, kFragment);
    gl.context().LinkProgram(program);
    GLint status = 0;
    gl.context().GetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == 0) {
      ok = false;
      if (error) {
        std::array<char, 512> log = {};
        gl.context().GetProgramInfoLog(
            program, static_cast<GLsizei>(log.size()), nullptr, log.data());
        *error = std::string("link: ") + log.data();
      }
    }
  }

  gl.context().DeleteShader(kVertex);
  gl.context().DeleteShader(kFragment);
  if (!ok) {
    if (program != 0) {
      gl.context().DeleteProgram(program);
    }
    return nullptr;
  }

  auto result = std::unique_ptr<GlProgram>(new GlProgram(&gl));
  result->id_ = program;
  return result;
}

GlProgram::~GlProgram() {
  if (id_ != 0) {
    gl_->context().DeleteProgram(id_);
    id_ = 0;
  }
}

void GlProgram::Use() const {
  gl_->context().UseProgram(id_);
}

int GlProgram::UniformLocation(const char* name) const {
  return gl_->context().GetUniformLocation(id_, name);
}

void GlProgram::SetInt(const char* name, int value) const {
  WithUniform(name,
              [this, value](int loc) { gl_->context().Uniform1i(loc, value); });
}

void GlProgram::SetFloat(const char* name, float value) const {
  WithUniform(name,
              [this, value](int loc) { gl_->context().Uniform1f(loc, value); });
}

void GlProgram::SetVec2(const char* name, float x, float y) const {
  WithUniform(name,
              [this, x, y](int loc) { gl_->context().Uniform2f(loc, x, y); });
}

void GlProgram::SetVec3(const char* name, float x, float y, float z) const {
  WithUniform(name, [this, x, y, z](int loc) {
    gl_->context().Uniform3f(loc, x, y, z);
  });
}

void GlProgram::SetVec4(const char* name, float x, float y, float z,
                        float w) const {
  WithUniform(name, [this, x, y, z, w](int loc) {
    gl_->context().Uniform4f(loc, x, y, z, w);
  });
}

void GlProgram::SetMat3(const char* name, const float* values) const {
  WithUniform(name, [this, values](int loc) {
    gl_->context().UniformMatrix3fv(loc, 1, false, values);
  });
}

void GlProgram::SetMat4(const char* name, const float* values) const {
  WithUniform(name, [this, values](int loc) {
    gl_->context().UniformMatrix4fv(loc, 1, false, values);
  });
}

}  // namespace avbase::gl
