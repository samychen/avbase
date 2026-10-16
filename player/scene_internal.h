// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// INTERNAL to the player library. Not part of the SDK surface: this is how
// PlayerImpl turns the sources it registered into the sink factory the renderer
// hands each secondary leg. Kept out of player/public so hosts never see the
// PublisherSink plumbing -- which is exactly what docs/14 stage 3 retired from
// the public SceneSource API (MakePublisherFactory).

#ifndef AVBASE_PLAYER_SCENE_INTERNAL_H_
#define AVBASE_PLAYER_SCENE_INTERNAL_H_

#include <memory>
#include <vector>

#include "media/base/video_renderer_sink.h"
#include "player/public/scene.h"

namespace avbase {

// Builds a sink factory that, for every secondary leg (source id >= 1), returns
// a sink that PUBLISHES the leg's decoded frame into the matching SceneSource
// -- which the scene has already plumbed into the host sink's engine. The
// vector is indexed by source id: sources[k] is fed by the leg whose source id
// is k + 1, because source 0 never reaches this factory (it takes the dedicated
// host sink first). Null entries are not expected; a leg whose id is out of
// range falls back to a NullVideoSink so a mis-registration degrades rather
// than crashes.
std::shared_ptr<media::VideoRendererSinkFactory>
CreateSecondarySinkFactory(std::vector<std::shared_ptr<SceneSource>> sources);

}  // namespace avbase

#endif  // AVBASE_PLAYER_SCENE_INTERNAL_H_
