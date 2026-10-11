// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "render/gl/gl_quad.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace avbase::gl {
namespace {

// Interleaved (x, y, u, v) per vertex, in triangle-strip order:
// bottom-left, bottom-right, top-left, top-right.
constexpr std::array<float, 16> kQuadUpright = {
    -1.0f, -1.0f, 0.0f, 1.0f,  //
    1.0f,  -1.0f, 1.0f, 1.0f,  //
    -1.0f, 1.0f,  0.0f, 0.0f,  //
    1.0f,  1.0f,  1.0f, 0.0f,
};

constexpr std::array<float, 16> kQuadFlipped = {
    -1.0f, -1.0f, 0.0f, 0.0f,  //
    1.0f,  -1.0f, 1.0f, 0.0f,  //
    -1.0f, 1.0f,  0.0f, 1.0f,  //
    1.0f,  1.0f,  1.0f, 1.0f,
};

constexpr int kFloatsPerVertex = 4;
constexpr int kVertexCount = 4;

// Bytes from one vertex to the next. Derived rather than written out, so a
// change to the layout above cannot leave the attribute setup describing the
// old one.
constexpr GLsizei kStride =
    kFloatsPerVertex * static_cast<GLsizei>(sizeof(float));

// Where the texcoord pair starts inside a vertex.
constexpr size_t kTexCoordOffset = 2 * sizeof(float);

// Turns a byte offset into the void* glVertexAttribPointer wants. GL reads that
// argument as an offset *into the bound buffer* rather than a pointer to
// dereference, so an integer has to become one -- there is no spelling of the
// call that avoids the conversion, which is why it is suppressed rather than
// restructured away. The uintptr_t hop is the portable spelling: casting the
// offset straight to a pointer is what performance-no-int-to-ptr is complaining
// about, and it warns on platforms where the two widths differ.
void* BufferOffset(size_t bytes) {
  // Trips performance-no-int-to-ptr. The name stays out of the parentheses
  // because cpplint rejects a category it does not know (readability/nolint).
  // NOLINTNEXTLINE
  return reinterpret_cast<void*>(static_cast<uintptr_t>(bytes));
}

}  // namespace

std::unique_ptr<GlQuad> GlQuad::Create(const GlApi& gl, bool flip_vertical) {
  auto quad = std::unique_ptr<GlQuad>(new GlQuad(&gl));

  gl.context().GenVertexArrays(1, &quad->vao_);
  gl.context().GenBuffers(1, &quad->vbo_);
  if (quad->vao_ == 0 || quad->vbo_ == 0) {
    return nullptr;
  }

  const float* vertices =
      flip_vertical ? kQuadFlipped.data() : kQuadUpright.data();
  gl.context().BindVertexArray(quad->vao_);
  gl.context().BindBuffer(GL_ARRAY_BUFFER, quad->vbo_);
  gl.context().BufferData(GL_ARRAY_BUFFER,
                          sizeof(float) * kFloatsPerVertex * kVertexCount,
                          vertices, GL_STATIC_DRAW);

  gl.context().EnableVertexAttribArray(0);
  gl.context().VertexAttribPointer(0, 2, GL_FLOAT, false, kStride, nullptr);
  gl.context().EnableVertexAttribArray(1);
  gl.context().VertexAttribPointer(1, 2, GL_FLOAT, false, kStride,
                                   BufferOffset(kTexCoordOffset));

  gl.context().BindVertexArray(0);
  gl.context().BindBuffer(GL_ARRAY_BUFFER, 0);
  return quad;
}

GlQuad::~GlQuad() {
  if (vbo_ != 0) {
    gl_->context().DeleteBuffers(1, &vbo_);
    vbo_ = 0;
  }
  if (vao_ != 0) {
    gl_->context().DeleteVertexArrays(1, &vao_);
    vao_ = 0;
  }
}

void GlQuad::Draw() const {
  gl_->context().BindVertexArray(vao_);
  gl_->context().DrawArrays(GL_TRIANGLE_STRIP, 0, kVertexCount);
}

}  // namespace avbase::gl
