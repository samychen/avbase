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

// The rotation, when ROTATE is defined, is applied to the TEXTURE COORDINATE
// and not to the quad. Those two are the same picture only when the rect is
// square: move the quad instead and a non-square rect turns the layer's area
// out from under the picture it is supposed to frame, so the layer would be
// rotated relative to its own rect rather than showing a rotated picture.
//
// uRotation is (cos, sin) of the turn, so the two lines below are the usual
// rotation matrix and there is no trigonometry in the shader: the four values
// a container can carry are exact, and std::cos(90 degrees) is not.
const char* const kQuadVertexShaderWithRectBody = R"(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
uniform vec4 uRect;
#ifdef ROTATE
uniform vec2 uRotation;
#endif
void main() {
    gl_Position = vec4(aPos * uRect.zw + uRect.xy, 0.0, 1.0);
#ifdef ROTATE
    vec2 c = aTexCoord - 0.5;
    vTexCoord = vec2(c.x * uRotation.x + c.y * uRotation.y,
                     c.y * uRotation.x - c.x * uRotation.y) + 0.5;
#else
    vTexCoord = aTexCoord;
#endif
}
)";

const char* const kVertexRotationDefine = "#define ROTATE\n";

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
