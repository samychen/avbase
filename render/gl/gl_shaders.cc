// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/gl/gl_shaders.h"

namespace avbase::gl {

std::string ShaderPrologue(const GlApi& gl) {
  if (gl.is_gles()) {
    if (gl.version_major() >= 3) {
      // ES 3.0: "es" is required, and a fragment shader has no default float
      // precision -- without these two lines every floating-point declaration
      // in the body is an error.
      return "#version 300 es\nprecision highp float;\n"
             "precision highp sampler2D;\n";
    }
    return "#version 100\nprecision mediump float;\n";
  }
  return "#version 330 core\n";
}

const char* const kQuadVertexShaderBody = R"(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

const char* const kQuadVertexShaderWithRectBody = R"(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
uniform vec4 uRect;
void main() {
    gl_Position = vec4(aPos * uRect.zw + uRect.xy, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

const char* const kTextureFragmentShaderBody = R"(
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTex;
void main() {
    fragColor = texture(uTex, vTexCoord);
}
)";

const char* const kTextureTintFragmentShaderBody = R"(
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec4 uTint;
void main() {
    fragColor = texture(uTex, vTexCoord) * uTint;
}
)";

const char* const kCrossFadeFragmentShaderBody = R"(
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uFrom;
uniform sampler2D uTo;
uniform float uProgress;
void main() {
    fragColor = mix(texture(uFrom, vTexCoord), texture(uTo, vTexCoord),
                    uProgress);
}
)";

}  // namespace avbase::gl
