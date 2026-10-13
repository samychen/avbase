// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_scene_engine.h"

#include <algorithm>
#include <utility>

#include "base/check.h"

namespace avbase::media {

GlSceneEngine::GlSceneEngine() = default;

GlSceneEngine::~GlSceneEngine() {
  TeardownCallback teardown;
  {
    base::AutoLock scoped(lock_);
    teardown = std::move(teardown_);
  }
  // Outside the lock, so a callback that reaches back into whatever owns this
  // engine does not deadlock against its own member's destructor. The
  // callback is told not to touch this object: it is already half gone.
  if (!teardown.is_null()) {
    std::move(teardown).Run();
  }
}

std::shared_ptr<GlLayerSource> GlSceneEngine::AddSource(std::string name,
                                                        int z) {
  auto source = std::make_shared<GlLayerSource>(std::move(name), z);
  base::AutoLock scoped(lock_);
  sources_.push_back(source);
  return source;
}

bool GlSceneEngine::RemoveSource(const std::shared_ptr<GlLayerSource>& source) {
  if (!source) {
    return false;
  }
  base::AutoLock scoped(lock_);
  const auto it = std::ranges::find(sources_, source);
  if (it == sources_.end()) {
    return false;
  }
  sources_.erase(it);
  retiring_.push_back(std::move(source));
  return true;
}

std::shared_ptr<GlLayerSource>
GlSceneEngine::FindSource(std::string_view name) const {
  base::AutoLock scoped(lock_);
  for (const auto& source : sources_) {
    if (source->name() == name) {
      return source;
    }
  }
  return nullptr;
}

size_t GlSceneEngine::source_count() const {
  base::AutoLock scoped(lock_);
  return sources_.size();
}

void GlSceneEngine::SetUpdateCallback(UpdateCallback callback) {
  base::AutoLock scoped(lock_);
  // Assigning a RepeatingCallback while the render thread may be copying or
  // running the previous one is why this is under the lock: the callback owns a
  // refcounted BindState, and "tear the old one down" racing "add a reference"
  // is a use-after-free that Release builds hide almost all of the time.
  update_ = std::move(callback);
}

void GlSceneEngine::SetTeardownCallback(TeardownCallback callback) {
  base::AutoLock scoped(lock_);
  teardown_ = std::move(callback);
}

bool GlSceneEngine::Sync(GlPresenter& presenter,
                         const base::scoped_refptr<VideoFrame>& main_frame,
                         PresentTiming* timing, GlScene** scene) {
  DCHECK(main_frame);
  // Snapshot under the lock, upload outside it: a source that publishes
  // hundred of frames a second must not be blocked by this frame's uploads,
  // and the snapshot is a handful of shared_ptr copies.
  UpdateCallback update;
  {
    base::AutoLock scoped(lock_);
    sync_list_.assign(sources_.begin(), sources_.end());
    retire_list_.swap(retiring_);
    // A snapshot, so the lock does not have to be held across the callback --
    // which may call back into this object (AddSource from inside a hook is
    // legal, and would deadlock). Copying a RepeatingCallback bumps a refcount;
    // it does not allocate.
    update = update_;
  }

  // A source retired since the last frame gives its layer back here, on the
  // thread that owns the scene, and before the early return below: a removal
  // must not wait for somebody to add a source again. Detach() with a null
  // scene is the "there was never a layer" case and is a no-op.
  for (const auto& source : retire_list_) {
    source->Detach(scene_);
  }
  retire_list_.clear();

  if (sync_list_.empty() && update.is_null()) {
    // Nothing to orchestrate: the caller takes the single-frame path, which is
    // also the path every measurement of it was taken on.
    *scene = nullptr;
    return true;
  }

  if (!scene_) {
    // Borrowed from the presenter, which owns it so the scene's GL objects are
    // deleted while the context is still current -- see EnsureScene().
    scene_ = presenter.EnsureScene();
    if (!scene_) {
      return false;
    }
    main_layer_ = scene_->AddVideoLayer(0);
    if (main_layer_ == GlScene::kNoLayer) {
      scene_ = nullptr;
      return false;
    }
  }

  if (!presenter.UploadLayer(*scene_, main_layer_, *main_frame, timing)) {
    return false;
  }
  for (const auto& source : sync_list_) {
    if (!source->Sync(presenter, *scene_, timing)) {
      return false;
    }
  }
  // The snapshot, not |update_|: the host may replace the hook on another
  // thread while this one runs, and what a frame draws with is the hook that
  // was installed when the frame started.
  if (!update.is_null()) {
    update.Run(presenter, *scene_, main_layer_, main_frame, timing);
  }
  *scene = scene_;
  return true;
}

bool GlSceneEngine::Compose(GlPresenter& presenter,
                            const base::scoped_refptr<VideoFrame>& main_frame,
                            PresentTiming* timing) {
  GlScene* scene = nullptr;
  if (!Sync(presenter, main_frame, timing, &scene)) {
    return false;
  }
  // Both branches compose into the canvas and stop there, which is the whole
  // point: the pixels are still readable and no window has been handed
  // anything yet.
  return scene != nullptr ? presenter.ComposeScene(*scene, timing)
                          : presenter.Compose(*main_frame, timing);
}

bool GlSceneEngine::Present(GlPresenter& presenter,
                            const base::scoped_refptr<VideoFrame>& main_frame,
                            PresentTiming* timing) {
  GlScene* scene = nullptr;
  if (!Sync(presenter, main_frame, timing, &scene)) {
    return false;
  }
  return scene != nullptr ? presenter.PresentScene(*scene, timing)
                          : presenter.Present(*main_frame, timing);
}

void GlSceneEngine::Shutdown() {
  base::AutoLock scoped(lock_);
  // No scene argument: whatever they belonged to is already gone or is the
  // presenter's to destroy. Detaching drops the frame each one holds, which is
  // the surface an imported layer is still pointing at.
  for (const auto& source : sources_) {
    source->Detach(nullptr);
  }
  for (const auto& source : retiring_) {
    source->Detach(nullptr);
  }
  // Drops the borrow only; the presenter destroys the scene. The order the
  // sinks rely on is: stop this first, drop the presenter second.
  scene_ = nullptr;
  main_layer_ = GlScene::kNoLayer;
}

}  // namespace avbase::media
