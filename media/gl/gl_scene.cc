// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_scene.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/logging.h"
#include "media/gl/gl_layer_geometry.h"

namespace avbase::media {

namespace {

// The rect a layer is drawn into once its content mode has had its say:
// |rect| when the mode is stretch, or |rect| narrowed about its centre to the
// picture's aspect when the mode is fit.
//
// It lives here rather than in the shader because fitting is a relationship
// between the picture and the destination's PIXEL grid, and the shader only
// ever sees normalised coordinates -- which cannot express "half of 1920 is
// not the same distance as half of 1080".
//
// The crop is folded into the picture's aspect: narrowing the source by
// (crop_w / crop_h) narrows what is drawn by the same factor. Using the
// UNCROPPED frame instead would letterbox a region the caller had already cut
// down, so a square crop of a wide clip would still come out with bars on it.
std::array<float, 4> FitRectInside(const std::array<float, 4>& rect,
                                   const std::array<float, 4>& crop,
                                   float src_aspect, int dst_width,
                                   int dst_height) {
  if (!(src_aspect > 0.0f) || dst_width <= 0 || dst_height <= 0) {
    return rect;
  }
  // The crop is part of what is being fitted: cutting the source down to
  // (crop_w x crop_h) of its stored width and height scales the picture's
  // aspect by (crop_w / crop_h). Fitting the UNCROPPED frame instead would
  // put bars around a region the caller had already narrowed.
  const float aspect = src_aspect * crop[2] / crop[3];
  // The rect's own shape is a PIXEL question, which is why the destination
  // size is a parameter: a rect covering half the width and half the height
  // of a 1920x1080 target is a 16:9 box, not a square.
  const float rect_aspect = rect[2] * static_cast<float>(dst_width) /
                            (rect[3] * static_cast<float>(dst_height));
  if (!(rect_aspect > 0.0f)) {
    return rect;
  }
  // Whichever axis overflows is the one that shrinks. Both branches are the
  // same expression with the axes exchanged, so the ratio is computed once
  // and only its meaning differs.
  const float scale =
      rect_aspect > aspect ? aspect / rect_aspect : rect_aspect / aspect;
  if (rect_aspect > aspect) {
    return {rect[0] + rect[2] * (1.0f - scale) * 0.5f, rect[1], rect[2] * scale,
            rect[3]};
  }
  return {rect[0], rect[1] + rect[3] * (1.0f - scale) * 0.5f, rect[2],
          rect[3] * scale};
}

}  // namespace

GlScene::GlScene(const gl::GlApi& gl, std::shared_ptr<gl::GlProgram> program,
                 std::shared_ptr<gl::GlProgram> rect_program)
    : gl_(&gl),
      program_(std::move(program)),
      rect_program_(std::move(rect_program)) {}

GlScene::~GlScene() = default;

std::unique_ptr<GlScene> GlScene::Create(const gl::GlApi& gl,
                                         std::string* error) {
  auto program = CreateVideoLayerProgram(gl, error);
  if (!program) {
    return nullptr;
  }
  // Built here with everything else that compiles a shader, for the same
  // reason: a driver that cannot compile the rectangle variant has to say so
  // when the scene is built, not in the middle of the first imported frame.
  auto rect_program = CreateVideoLayerRectProgram(gl, error);
  if (!rect_program) {
    return nullptr;
  }
  return CreateWithPrograms(
      gl, std::shared_ptr<gl::GlProgram>(std::move(program)),
      std::shared_ptr<gl::GlProgram>(std::move(rect_program)), error);
}

std::unique_ptr<GlScene> GlScene::CreateWithPrograms(
    const gl::GlApi& gl, std::shared_ptr<gl::GlProgram> program,
    std::shared_ptr<gl::GlProgram> rect_program, std::string* error) {
  auto quad = gl::GlQuad::Create(gl);
  if (!quad) {
    if (error) {
      *error = "scene: quad creation failed";
    }
    return nullptr;
  }
  // unique_ptr rather than make_unique because the constructor is private.
  auto scene = std::unique_ptr<GlScene>(
      new GlScene(gl, std::move(program), std::move(rect_program)));
  scene->quad_ = std::move(quad);
  return scene;
}

GlScene::Layer* GlScene::Find(LayerId id) {
  if (id == kNoLayer) {
    return nullptr;
  }
  for (Layer& layer : layers_) {
    if (layer.id == id) {
      return &layer;
    }
  }
  return nullptr;
}

const GlScene::Layer* GlScene::Find(LayerId id) const {
  if (id == kNoLayer) {
    return nullptr;
  }
  for (const Layer& layer : layers_) {
    if (layer.id == id) {
      return &layer;
    }
  }
  return nullptr;
}

GlScene::LayerId GlScene::AddVideoLayer(int z) {
  auto video = GlVideoLayer::Create(*gl_);
  if (!video) {
    return kNoLayer;
  }
  Layer layer;
  layer.id = next_id_++;
  layer.z = z;
  layer.video = std::move(video);
  layers_.push_back(std::move(layer));
  return layers_.back().id;
}

bool GlScene::RemoveLayer(LayerId id) {
  for (auto it = layers_.begin(); it != layers_.end(); ++it) {
    if (it->id == id) {
      layers_.erase(it);
      return true;
    }
  }
  return false;
}

bool GlScene::SetLayerZ(LayerId id, int z) {
  Layer* layer = Find(id);
  if (!layer) {
    return false;
  }
  layer->z = z;
  return true;
}

bool GlScene::SetLayerVisible(LayerId id, bool visible) {
  Layer* layer = Find(id);
  if (!layer) {
    return false;
  }
  layer->visible = visible;
  return true;
}

bool GlScene::SetLayerOpacity(LayerId id, float opacity) {
  Layer* layer = Find(id);
  if (!layer) {
    return false;
  }
  // The setter half of the policy in gl_scene.h: reject and keep the previous
  // value, because this call can report the failure.
  if (!std::isfinite(opacity)) {
    return false;
  }
  layer->opacity = std::clamp(opacity, 0.0f, 1.0f);
  return true;
}

bool GlScene::SetLayerRect(LayerId id, float x, float y, float w, float h) {
  Layer* layer = Find(id);
  if (!layer) {
    return false;
  }
  // Same policy as SetLayerOpacity; see gl_scene.h.
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) ||
      !std::isfinite(h)) {
    return false;
  }
  layer->rect = {x, y, w, h};
  return true;
}

bool GlScene::SetLayerRotation(LayerId id, std::optional<int> degrees) {
  Layer* layer = Find(id);
  if (!layer) {
    return false;
  }
  // Snap at the setter so LayerRotation() reports the angle the layer is
  // ACTUALLY drawn at: a caller that asks must not be told 0 when the picture
  // is turned 90 and the value it passed was 89.
  if (degrees) {
    layer->rotation_override = SnapLayerRotation(*degrees);
  } else {
    layer->rotation_override = std::nullopt;
  }
  return true;
}

int GlScene::LayerRotation(LayerId id) const {
  const Layer* layer = Find(id);
  if (!layer) {
    return 0;
  }
  return layer->rotation_override.value_or(layer->frame_rotation);
}

bool GlScene::SetLayerSourceRect(LayerId id, float x, float y, float w,
                                 float h) {
  Layer* layer = Find(id);
  if (!layer) {
    return false;
  }
  // Rejected rather than clamped, on the getters' own terms (see gl_scene.h):
  // a crop is either a region of the picture or it is a mistake, and there is
  // no third reading in which "partly outside" means something.
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) ||
      !std::isfinite(h) || w <= 0.0f || h <= 0.0f || x < 0.0f || y < 0.0f ||
      x + w > 1.0f || y + h > 1.0f) {
    return false;
  }
  layer->source_rect = {x, y, w, h};
  return true;
}

const std::array<float, 4>& GlScene::source_rect(LayerId id) const {
  static const std::array<float, 4> kFull = {0.0f, 0.0f, 1.0f, 1.0f};
  const Layer* layer = Find(id);
  return layer ? layer->source_rect : kFull;
}

bool GlScene::SetLayerContentMode(LayerId id, LayerContentMode mode) {
  Layer* layer = Find(id);
  if (!layer) {
    return false;
  }
  layer->content_mode = mode;
  return true;
}

LayerContentMode GlScene::content_mode(LayerId id) const {
  const Layer* layer = Find(id);
  return layer ? layer->content_mode : LayerContentMode::kStretch;
}

bool GlScene::SetBackgroundColor(float r, float g, float b, float a) {
  // Rejected rather than clamped; a cleared alpha outside [0,1] would produce
  // a target nobody can reason about rather than a diagnosable call.
  if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) ||
      !std::isfinite(a)) {
    return false;
  }
  background_ = {r, g, b, a};
  return true;
}

std::array<float, 4> GlScene::FittedRect(const Layer& layer, int dst_width,
                                         int dst_height) const {
  if (layer.content_mode != LayerContentMode::kFit) {
    return layer.rect;
  }
  // |source_aspect| already has the rotation folded in, so a quarter turn
  // needs no special case here: it just makes the picture taller than wide.
  return FitRectInside(layer.rect, layer.source_rect,
                       layer.video->source_aspect(), dst_width, dst_height);
}

bool GlScene::Upload(LayerId id, const VideoFrame& frame,
                     GlHwTextureImporter* importer) {
  Layer* layer = Find(id);
  if (!layer) {
    return false;
  }
  // Captured here, not in Compose(): the frame is the only witness to its own
  // rotation, and the caller is free to release it the moment this returns.
  layer->frame_rotation = frame.rotation();
  return layer->video->Upload(frame, importer);
}

UploadPath GlScene::last_upload_path(LayerId id) const {
  for (const Layer& layer : layers_) {
    if (layer.id == id) {
      return layer.video->last_upload_path();
    }
  }
  return UploadPath::kNone;
}

void GlScene::Compose(int dst_width, int dst_height) {
  // Order once per frame rather than on every SetLayerZ: dragging a layer
  // around issues many moves and presents once.
  //
  // stable_sort so layers sharing a z keep the order they were added, which is
  // the only ordering the caller can predict. An unstable sort would let two
  // equal-z layers swap between frames on nothing but their addresses.
  draw_order_.clear();
  for (size_t i = 0; i < layers_.size(); ++i) {
    const Layer& layer = layers_[i];
    if (layer.visible && layer.video->ready()) {
      draw_order_.push_back(i);
    }
  }
  // sort, not stable_sort, and the INDEX is the tiebreak: draw_order_ holds
  // ascending indices, so "(z, index)" orders equal-z layers by insertion --
  // which is exactly what stability would have given, stated instead of
  // borrowed. It matters because std::stable_sort needs a heap buffer for its
  // merge, and this runs once per displayed frame: Compose() is documented as
  // not allocating, and scratch vectors made members do not count for much if
  // the sort under them allocates anyway.
  std::sort(draw_order_.begin(), draw_order_.end(), [this](size_t a, size_t b) {
    if (layers_[a].z != layers_[b].z) {
      return layers_[a].z < layers_[b].z;
    }
    return a < b;
  });

  // Fitting needs the target's pixel grid, so a caller that binds one has to
  // say how big it is. Told once per scene rather than once per frame: the
  // size is a property of the target, not of the moment.
  if (dst_width <= 0 || dst_height <= 0) {
    LOG(WARNING) << "avbase.scene: Compose() without a destination size; "
                    "every layer will stretch to its rect instead of fitting";
  }

  // Cleared to the scene's own background colour, so an uncovered area of a
  // composition looks exactly like the letterboxing around a video: the
  // composition's background, not a hole.
  gl_->context().ClearColor(background_[0], background_[1], background_[2],
                            background_[3]);
  gl_->context().Clear(GL_COLOR_BUFFER_BIT);

  // Blending on for the whole pass rather than per layer: an opaque layer
  // (alpha 1) blended with SRC_ALPHA/ONE_MINUS_SRC_ALPHA comes out identical
  // to no blending at all, so the branch would only add a second path for the
  // common case.
  gl_->context().Enable(GL_BLEND);
  gl_->context().BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // The program is chosen per layer, because a scene can hold both an owned
  // frame (GL_TEXTURE_2D) and an imported one (GL_TEXTURE_RECTANGLE) at the
  // same time -- a hardware-decoded stream over a software-decoded overlay is
  // the case this exists for. `bound` caches the last one so a scene whose
  // layers all agree pays one glUseProgram for the whole frame, and uRect is
  // set after the bind either way, because uniform state belongs to the
  // program and a switch would otherwise keep the previous layer's placement.
  gl::GlProgram* bound = nullptr;
  for (size_t index : draw_order_) {
    const Layer& layer = layers_[index];
    gl::GlProgram* program =
        layer.video->texture_target() == GL_TEXTURE_RECTANGLE
            ? rect_program_.get()
            : program_.get();
    if (program != bound) {
      program->Use();
      bound = program;
    }
    // The rect the layer is DRAWN into, which is not always the rect it was
    // GIVEN: a layer asked to fit has its box narrowed to its picture's shape,
    // and the remainder is left showing the background cleared above.
    const std::array<float, 4> rect = FittedRect(layer, dst_width, dst_height);
    float uv_rect[4];
    NormalisedRectToUvRect(rect[0], rect[1], rect[2], rect[3], uv_rect);
    program->SetVec4("uRect", uv_rect[0], uv_rect[1], uv_rect[2], uv_rect[3]);
    // Set after the bind, for the same reason as uRect: uniform state belongs
    // to the program, and the two layer variants share this one.
    float cos_sin[2];
    LayerRotationToCosSin(
        layer.rotation_override.value_or(layer.frame_rotation), cos_sin);
    program->SetVec2("uRotation", cos_sin[0], cos_sin[1]);
    program->SetVec4("uSrcRect", layer.source_rect[0], layer.source_rect[1],
                     layer.source_rect[2], layer.source_rect[3]);
    layer.video->Bind(*program, layer.opacity);
    quad_->Draw();
  }

  gl_->context().Disable(GL_BLEND);
}

}  // namespace avbase::media
