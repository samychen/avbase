// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_layer_source.h"

#include <utility>

namespace avbase::media {

GlLayerSource::GlLayerSource(std::string name, int z)
    : name_(std::move(name)), z_(z) {}
GlLayerSource::~GlLayerSource() = default;

void GlLayerSource::Publish(base::scoped_refptr<VideoFrame> frame) {
  queue_.Publish(std::move(frame));
}

GlScene::LayerId GlLayerSource::EnsureLayer(GlScene& scene) {
  if (layer_ == GlScene::kNoLayer) {
    layer_ = scene.AddVideoLayer(z_);
  }
  return layer_;
}

bool GlLayerSource::Sync(GlPresenter& presenter, GlScene& scene,
                         PresentTiming* timing) {
  const GlScene::LayerId id = EnsureLayer(scene);
  if (id == GlScene::kNoLayer) {
    return false;
  }
  base::scoped_refptr<VideoFrame> frame = queue_.Consume();
  if (!frame) {
    // Nothing new since the last Sync. The layer still holds the previous
    // picture -- including an imported one, whose surface is kept alive by the
    // reference below -- so this is the ordinary "source is running slower
    // than the display" case and not a failure.
    return true;
  }
  if (!presenter.UploadLayer(scene, id, *frame, timing)) {
    return false;
  }
  displayed_ = std::move(frame);
  return true;
}

void GlLayerSource::Detach(GlScene* scene) {
  if (scene && layer_ != GlScene::kNoLayer) {
    scene->RemoveLayer(layer_);
  }
  layer_ = GlScene::kNoLayer;
  displayed_ = nullptr;
}

uint64_t GlLayerSource::frames_published() const {
  return queue_.published_frames();
}

uint64_t GlLayerSource::frames_consumed() const {
  return queue_.consumed_frames();
}

uint64_t GlLayerSource::frames_superseded() const {
  return queue_.superseded_frames();
}

}  // namespace avbase::media
