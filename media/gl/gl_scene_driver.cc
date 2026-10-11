// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_scene_driver.h"

#include <string>
#include <utility>

#include "base/logging.h"
#include "media/gl/gl_present.h"

namespace avbase::media {

GlSceneDriver::GlSceneDriver() = default;

GlSceneDriver::~GlSceneDriver() = default;

void GlSceneDriver::SetUpdateCallback(UpdateCallback callback) {
  update_ = std::move(callback);
}

bool GlSceneDriver::Present(GlPresenter& presenter,
                            const base::scoped_refptr<VideoFrame>& frame,
                            PresentTiming* timing) {
  if (update_.is_null()) {
    return presenter.Present(*frame, timing);
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
  if (!presenter.UploadLayer(*scene_, main_layer_, *frame, timing)) {
    return false;
  }
  update_.Run(presenter, *scene_, main_layer_, frame, timing);
  return presenter.PresentScene(*scene_, timing);
}

void GlSceneDriver::Shutdown() {
  // Drops the borrow only; the presenter destroys the scene. The order the
  // sinks rely on is: stop this first, drop the presenter second.
  scene_ = nullptr;
  main_layer_ = GlScene::kNoLayer;
}

}  // namespace avbase::media
