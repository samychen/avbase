// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/scene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "base/functional/bind.h"
#include "media/gl/gl_layer_source.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"
#include "media/gl/gl_scene_engine.h"

namespace avbase {
namespace {

// Every setter's first question, asked once here rather than nine times with
// nine chances to get it slightly differently. Not finite means the caller
// computed something broken upstream; a camera NaN reaching a layer's rect is
// a bug to report, not a value to make usable.
bool AllFinite(std::initializer_list<float> values) {
  return std::ranges::all_of(values, [](float v) { return std::isfinite(v); });
}

}  // namespace

// ---- SceneFrame ------------------------------------------------------------

void SceneFrame::Begin(media::GlScene* scene, media::GlPresenter* presenter,
                       media::PresentTiming* timing, uint64_t main_layer) {
  scene_ = scene;
  presenter_ = presenter;
  timing_ = timing;
  main_layer_ = main_layer;
}

void SceneFrame::End() {
  // This is the guarantee behind "valid only inside the callback": every
  // SceneLayer made from this frame now fails its checks instead of reading
  // through a pointer to an object whose lifetime ended one return ago.
  scene_ = nullptr;
  presenter_ = nullptr;
  timing_ = nullptr;
  main_layer_ = 0;
}

SceneLayer SceneFrame::main_layer() {
  return live() ? SceneLayer(this, main_layer_) : SceneLayer();
}

SceneLayer SceneFrame::AddLayer(int z) {
  if (!live()) {
    return SceneLayer();
  }
  return SceneLayer(this, scene_->AddVideoLayer(z));
}

bool SceneFrame::RemoveLayer(SceneLayer layer) {
  if (!live() || layer.frame_ != this) {
    return false;
  }
  return scene_->RemoveLayer(layer.id_);
}

size_t SceneFrame::layer_count() const {
  return live() ? scene_->layer_count() : 0;
}

SceneLayer SceneFrame::layer_of(const SceneSource& source) {
  if (!live()) {
    return SceneLayer();
  }
  base::AutoLock scoped(source.lock_);
  if (!source.source_ || !source.source_->has_layer()) {
    return SceneLayer();
  }
  return SceneLayer(this, source.source_->layer());
}

SceneLayer SceneFrame::layer_with_id(uint64_t id) {
  if (!live() || id == media::GlScene::kNoLayer || !scene_->HasLayer(id)) {
    return SceneLayer();
  }
  return SceneLayer(this, id);
}

// ---- SceneLayer ------------------------------------------------------------

bool SceneLayer::valid() const {
  return frame_ != nullptr && frame_->live() && id_ != media::GlScene::kNoLayer;
}

bool SceneLayer::SetRect(float x, float y, float w, float h) {
  if (!valid()) {
    return false;
  }
  return frame_->scene()->SetLayerRect(id_, x, y, w, h);
}

bool SceneLayer::SetZ(int z) {
  return valid() && frame_->scene()->SetLayerZ(id_, z);
}

bool SceneLayer::SetOpacity(float opacity) {
  return valid() && frame_->scene()->SetLayerOpacity(id_, opacity);
}

bool SceneLayer::SetVisible(bool visible) {
  return valid() && frame_->scene()->SetLayerVisible(id_, visible);
}

bool SceneLayer::SetRotation(std::optional<int> degrees) {
  return valid() && frame_->scene()->SetLayerRotation(id_, degrees);
}

bool SceneLayer::SetSourceRect(float x, float y, float w, float h) {
  if (!valid()) {
    return false;
  }
  return frame_->scene()->SetLayerSourceRect(id_, x, y, w, h);
}

bool SceneLayer::SetContentMode(SceneContentMode mode) {
  if (!valid()) {
    return false;
  }
  return frame_->scene()->SetLayerContentMode(
      id_, mode == SceneContentMode::kFit ? media::LayerContentMode::kFit
                                          : media::LayerContentMode::kStretch);
}

int SceneLayer::rotation() const {
  return valid() ? frame_->scene()->LayerRotation(id_) : 0;
}

bool SceneLayer::Upload(const media::VideoFrame& frame) {
  if (!valid()) {
    return false;
  }
  // Through the presenter rather than straight into the scene, so this upload
  // lands in the frame's upload_ms next to the main layer's: a second layer
  // that looked free would be the one number nobody would think to check.
  return frame_->presenter()->UploadLayer(*frame_->scene(), id_, frame,
                                          frame_->timing());
}

// ---- Scene -----------------------------------------------------------------

Scene::Scene() = default;

// Every engine unbinds itself before it dies, so the defaulted destructor has
// nothing to release -- which is the point of the teardown callback. What
// would be a leak of the pointer is instead a fact nobody has to remember.
Scene::~Scene() = default;

std::shared_ptr<SceneSource> Scene::AddSource(std::string name, int z) {
  // Not make_shared: that would need the constructor to be public, and the
  // whole point is that only a Scene may make one -- a SceneSource nobody
  // registered is a source nobody can ever plumb anywhere.
  std::shared_ptr<SceneSource> source(new SceneSource(std::move(name), z));
  {
    base::AutoLock scoped(lock_);
    sources_.push_back(source);
    PlumbLocked(*source);
  }
  return source;
}

bool Scene::RemoveSource(const std::shared_ptr<SceneSource>& source) {
  if (!source) {
    return false;
  }
  base::AutoLock scoped(lock_);
  const auto it = std::ranges::find(sources_, source);
  if (it == sources_.end()) {
    return false;
  }
  sources_.erase(it);
  std::shared_ptr<media::GlLayerSource> bound;
  {
    base::AutoLock source_lock(source->lock_);
    bound = source->source_;
  }
  if (engine_ != nullptr && bound) {
    engine_->RemoveSource(bound);
  }
  source->Retire();
  return true;
}

std::shared_ptr<SceneSource> Scene::FindSource(std::string_view name) const {
  base::AutoLock scoped(lock_);
  for (const auto& source : sources_) {
    if (source->name() == name) {
      return source;
    }
  }
  return nullptr;
}

size_t Scene::source_count() const {
  base::AutoLock scoped(lock_);
  return sources_.size();
}

void Scene::SetUpdateCallback(SceneUpdateCallback callback) {
  base::AutoLock scoped(lock_);
  hook_ = std::move(callback);
  InstallHookLocked();
}

bool Scene::SetBackgroundColor(float r, float g, float b, float a) {
  if (!AllFinite({r, g, b, a})) {
    return false;
  }
  base::AutoLock scoped(lock_);
  background_ = {r, g, b, a};
  background_set_ = true;
  InstallHookLocked();
  return true;
}

std::string Scene::Describe() const {
  size_t sources = 0;
  bool hooked = false;
  bool background = false;
  bool bound = false;
  {
    base::AutoLock scoped(lock_);
    sources = sources_.size();
    hooked = !hook_.is_null();
    background = background_set_;
    bound = engine_ != nullptr;
  }
  std::string out = bound ? "bound" : "unbound";
  if (!bound) {
    out += " (the running pipeline's sink cannot composite";
    if (sources != 0 || hooked) {
      out += "; ";
      out += std::to_string(sources);
      out += " source(s) and ";
      out += hooked ? "a hook" : "no hook";
      out += " are waiting to be plumbed in";
    }
    out += ")";
  } else {
    out += ", ";
    out += std::to_string(sources);
    out += " source(s), ";
    out += hooked ? "hook installed" : "no hook";
    if (background) {
      out += ", background set";
    }
  }
  return out;
}

void Scene::Bind(media::GlSceneEngine* engine) {
  base::AutoLock scoped(lock_);
  if (engine_ != nullptr) {
    // Two live pipelines claiming one scene. Retire the old one's plumbing
    // rather than calling into it: it belongs to whoever built it, and that
    // someone is mid-teardown.
    for (const auto& source : sources_) {
      source->Retire();
    }
  }
  engine_ = engine;
  if (engine_ != nullptr) {
    // Unretained, and safe because of an ordering the player guarantees: it
    // tears every pipeline down before it destroys this object, and this runs
    // from the engine's destructor, which is inside that teardown.
    engine_->SetTeardownCallback(
        base::BindOnce(&Scene::OnEngineGone, base::Unretained(this)));
  }
  InstallHookLocked();
  for (const auto& source : sources_) {
    PlumbLocked(*source);
  }
}

void Scene::OnEngineGone() {
  base::AutoLock scoped(lock_);
  // Reports rather than infers, which is what makes it reliable: see the note
  // on GlSceneEngine::SetTeardownCallback for the paths the player does not
  // get to announce.
  engine_ = nullptr;
  for (const auto& source : sources_) {
    source->Retire();
  }
}

void Scene::PlumbLocked(SceneSource& source) {
  if (engine_ == nullptr) {
    source.Retire();
    return;
  }
  source.Bind(engine_->AddSource(source.name(), source.z()));
}

void Scene::InstallHookLocked() {
  if (engine_ == nullptr) {
    return;
  }
  // The engine's own fast path is worth not paying for: with no sources and no
  // hook it hands the frame to the single-frame presenter, and that is the
  // path every measurement of single-video playback was taken on. So the
  // trampoline is installed only when there is something for it to do.
  if (hook_.is_null() && !background_set_) {
    engine_->SetUpdateCallback(media::GlSceneEngine::UpdateCallback());
    return;
  }
  engine_->SetUpdateCallback(
      base::BindRepeating(&Scene::RunHook, base::Unretained(this)));
}

// static
void Scene::RunHook(Scene* self, media::GlPresenter& presenter,
                    media::GlScene& gl_scene, uint64_t main_layer,
                    const base::scoped_refptr<media::VideoFrame>& frame,
                    media::PresentTiming* timing) {
  SceneUpdateCallback hook;
  // Braced: the array is assigned under the lock below, but an uninitialised
  // float[4] read between here and there is a real (if unlikely) path --
  // cppcoreguidelines-pro-type-member-init is right about this one.
  std::array<float, 4> background{};
  bool background_set = false;
  {
    base::AutoLock scoped(self->lock_);
    hook = self->hook_;
    background = self->background_;
    background_set = self->background_set_;
  }
  if (background_set) {
    gl_scene.SetBackgroundColor(background[0], background[1], background[2],
                                background[3]);
  }
  if (hook.is_null()) {
    return;
  }
  SceneFrame callback_frame;
  callback_frame.Begin(&gl_scene, &presenter, timing, main_layer);
  hook.Run(callback_frame, frame);
  callback_frame.End();
}

}  // namespace avbase
