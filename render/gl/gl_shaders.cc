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

// The vertex stage for a placed quad, carrying TWO optional transforms, each
// behind its own define so a program that does not set one does not have it:
//
//   ROTATE -- turn the picture about the quad's middle by uRotation;
//   CROP   -- fill the quad from uSrcRect (xy = origin, zw = size) alone, in
//             normalised SOURCE coordinates over the frame as the decoder
//             stored it.
//
// The crop names a region of the STORED picture, because that is the only frame
// the caller ever had a look at -- and yet it is applied SECOND below. This
// stage maps SCREEN back to SOURCE, so the step the viewer sees last is the
// one computed first: undoing the turn is what recovers "the picture as
// stored", and only then do the crop's four numbers mean anything in it. Put
// the crop the other way round and the crop BOX itself gets turned about the
// picture's middle, which sweeps it to a different region -- and for a crop
// that is not square, outside the frame altogether. Both orders agree on an
// upright clip, so this only separates on material carrying a quarter turn.
//
// ROTATE works on the TEXTURE COORDINATE and not on the quad. Those two are the
// same picture only when the rect is square: move the quad instead and a
// non-square rect turns the layer's area out from under the picture it is
// supposed to frame, so the layer would be rotated relative to its own rect
// rather than showing a rotated picture.
//
// uRotation is (cos, sin) of the turn, so the two lines below are the usual
// rotation matrix and there is no trigonometry in the shader: the four values
// a container can carry are exact, and std::cos(90 degrees) is not.
const char* const kQuadVertexShaderWithRectBody = R"(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
uniform vec4 uRect;
#ifdef CROP
uniform vec4 uSrcRect;
#endif
#ifdef ROTATE
uniform vec2 uRotation;
#endif
void main() {
    gl_Position = vec4(aPos * uRect.zw + uRect.xy, 0.0, 1.0);
    // Centred on the middle of the quad: that is the point both steps below
    // are measured about, the turn because turning happens about the middle
    // and the crop because it is placed by its own.
    vec2 t = aTexCoord - 0.5;
#ifdef ROTATE
    t = vec2(t.x * uRotation.x + t.y * uRotation.y,
             t.y * uRotation.x - t.x * uRotation.y);
#endif
#ifdef CROP
    // Out of the unit box and into the crop: its size scales, its middle
    // places. See the header on why this comes after the turn and not before.
    t = t * uSrcRect.zw + uSrcRect.xy + uSrcRect.zw * 0.5;
#else
    t = t + 0.5;
#endif
    vTexCoord = t;
}
)";

const char* const kVertexRotationDefine = "#define ROTATE\n";

const char* const kVertexCropDefine = "#define CROP\n";

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

// The weight is a STEP along one axis rather than a constant, so the two sides
// are joined by a moving edge instead of by fading. |uDirection| says which way
// the edge travels, as a unit vector over the picture: (1, 0) moves it
// rightwards, so |to| comes in from the left.
//
// A hard edge and not a soft one: a wipe is a CUT THAT MOVES. smoothstep()
// would make the band's width a second parameter nobody had agreed on, and one
// a pixel assertion could not tell apart from a small change in progress.
const char* const kWipeFragmentShaderBody = R"(
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uFrom;
uniform sampler2D uTo;
uniform float uProgress;
uniform vec2 uDirection;
void main() {
    // How far this pixel lies along the travel axis, remapped from [-0.5, 0.5]
    // to [0, 1] so that |uProgress| reads as "how much of the picture the
    // edge has crossed".
    float d = dot(vTexCoord - 0.5, uDirection) + 0.5;
    fragColor = mix(texture(uFrom, vTexCoord), texture(uTo, vTexCoord),
                    step(d, uProgress));
}
)";

}  // namespace avbase::gl
