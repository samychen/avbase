// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/player_qt/gl_video_renderer.h"

#include <utility>

#include "base/logging.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_layer_geometry.h"
#include "media/gl/gl_video_layer.h"
#include "platform/qt/qt_gl_context.h"
#include "render/gl/gl_api.h"
#include "render/gl/gl_program.h"
#include "render/gl/gl_quad.h"

class QOpenGLContext;

namespace avbase::apps {

namespace {

// #0c0c0e, the same background the software item paints. Matching it is not
// cosmetic: the two paths are compared pixel by pixel, and a different
// letterbox colour would show up as a difference everywhere the picture is
// not.
constexpr float kBackgroundR = 12.0f / 255.0f;
constexpr float kBackgroundG = 12.0f / 255.0f;
constexpr float kBackgroundB = 14.0f / 255.0f;

}  // namespace

GlVideoRenderer::GlVideoRenderer() = default;

GlVideoRenderer::~GlVideoRenderer() {
  // Dependency order, all of it while the context is still current: the quad,
  // the programs and the layer all borrow the GlApi.
  quad_.reset();
  program_.reset();
  rect_program_.reset();
  layer_.reset();
  gl_.reset();
  context_.reset();
}

bool GlVideoRenderer::Init(void* qt_context, int drawable_width,
                           int drawable_height, std::string* error) {
  if (qt_context == nullptr) {
    if (error != nullptr) {
      *error = "no current OpenGL context";
    }
    return false;
  }
  // A static_cast from void* to a pointer to an incomplete type: the header
  // only forward-declares QOpenGLContext, and that is the whole point of the
  // seam -- this file must not see Qt's GL headers.
  auto* context = static_cast<QOpenGLContext*>(qt_context);
  context_ = std::make_unique<media::QtGLContext>(context, drawable_width,
                                                  drawable_height);
  gl_ = std::make_unique<gl::GlApi>();
  if (!gl_->Load(context_.get(), error)) {
    gl_.reset();
    context_.reset();
    return false;
  }
  program_ = media::CreateVideoLayerProgram(*gl_, error);
  if (!program_) {
    gl_.reset();
    context_.reset();
    return false;
  }
  layer_ = media::GlVideoLayer::Create(*gl_);
  if (!layer_) {
    if (error != nullptr) {
      *error = "GlVideoLayer::Create failed";
    }
    program_.reset();
    gl_.reset();
    context_.reset();
    return false;
  }
  // flip_vertical=false: the decoder hands us rows top-down and NDC y=-1 is the
  // bottom of the viewport, so the unflipped quad is the upright one. If this
  // is ever wrong the picture is upside down and nothing else is, which is why
  // the app ships a pixel comparison instead of trusting the comment.
  quad_ = gl::GlQuad::Create(*gl_, /*flip_vertical=*/false);
  if (!quad_) {
    if (error != nullptr) {
      *error = "GlQuad::Create failed";
    }
    layer_.reset();
    program_.reset();
    gl_.reset();
    context_.reset();
    return false;
  }
  const auto& gl = gl_->context();
  // GetString returns const GLubyte*; streaming it as-is prints the POINTER.
  const char* version = reinterpret_cast<const char*>(gl.GetString(GL_VERSION));
  const char* renderer =
      reinterpret_cast<const char*>(gl.GetString(GL_RENDERER));
  LOG(INFO) << "qt-gl: " << version << " / " << renderer;
  if (renderer != nullptr) {
    renderer_name_ = renderer;
  }
  return true;
}

bool GlVideoRenderer::Upload(const media::VideoFrame& frame) {
  if (!layer_) {
    return false;
  }
  return layer_->Upload(frame);
}

bool GlVideoRenderer::has_picture() const {
  return layer_ && layer_->ready();
}

void GlVideoRenderer::Paint(int width, int height, int rotation_degrees) {
  if (!has_picture() || !quad_ || width <= 0 || height <= 0) {
    return;
  }
  const auto& gl = gl_->context();
  gl.Viewport(0, 0, width, height);
  // Stated rather than assumed: QSG's previous item may have left any of these
  // enabled, and each one silently changes what this draw produces.
  gl.Disable(GL_DEPTH_TEST);
  gl.Disable(GL_BLEND);
  gl.Disable(GL_CULL_FACE);
  gl.Disable(GL_SCISSOR_TEST);
  gl.ClearColor(kBackgroundR, kBackgroundG, kBackgroundB, 1.0f);
  gl.Clear(GL_COLOR_BUFFER_BIT);
  DrawFitted(width, height, rotation_degrees);
}

void GlVideoRenderer::DrawFitted(int width, int height, int rotation_degrees) {
  // An imported frame lives on GL_TEXTURE_RECTANGLE and only the rect variant
  // can read it. Nothing installs an importer today, so this branch is dead
  // code that costs one comparison -- kept because the alternative is a silent
  // black picture the day someone does install one.
  gl::GlProgram* program = layer_->texture_target() == GL_TEXTURE_RECTANGLE
                               ? RectProgram()
                               : program_.get();
  if (program == nullptr) {
    return;
  }

  // Fit the picture inside the surface, keeping its aspect. source_aspect()
  // already has the display rotation folded in -- a quarter turn swaps the
  // axes -- so a portrait-held phone clip lands in a tall box without a second
  // special case, and the turned picture is not squeezed into the unrotated
  // one.
  const float aspect = layer_->source_aspect();
  float draw_w = static_cast<float>(width);
  float draw_h = static_cast<float>(height);
  if (aspect > 0.0f) {
    if (aspect > draw_w / draw_h) {
      draw_h = draw_w / aspect;
    } else {
      draw_w = draw_h * aspect;
    }
  }
  const float inv_w = 1.0f / static_cast<float>(width);
  const float inv_h = 1.0f / static_cast<float>(height);
  const float nx = (static_cast<float>(width) - draw_w) * 0.5f * inv_w;
  const float ny = (static_cast<float>(height) - draw_h) * 0.5f * inv_h;
  // Normalised (top-left origin) in, NDC (centre + half extent) out. The two
  // spellings agree only at full-target -- the case a hand-written conversion
  // gets right by accident and then gets wrong the first time something is
  // letterboxed.
  float rect[4];
  media::NormalisedRectToUvRect(nx, ny, draw_w * inv_w, draw_h * inv_h, rect);

  program->Use();
  program->SetVec4("uRect", rect[0], rect[1], rect[2], rect[3]);
  float cos_sin[2];
  media::LayerRotationToCosSin(rotation_degrees, cos_sin);
  program->SetVec2("uRotation", cos_sin[0], cos_sin[1]);
  // Set every frame: uSrcRect is shared with any caller that crops, and an
  // unset vec4 is (0,0,0,0) -- an empty box, not "the whole picture".
  program->SetVec4("uSrcRect", 0.0f, 0.0f, 1.0f, 1.0f);
  layer_->Bind(*program, 1.0f);
  quad_->Draw();
}

gl::GlProgram* GlVideoRenderer::RectProgram() {
  if (!rect_program_ && gl_) {
    std::string error;
    rect_program_ = media::CreateVideoLayerRectProgram(*gl_, &error);
    if (!rect_program_) {
      LOG(ERROR) << "qt-gl: rectangle-texture program failed: " << error;
    }
  }
  return rect_program_.get();
}

}  // namespace avbase::apps
