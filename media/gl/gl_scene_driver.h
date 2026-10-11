// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_GL_GL_SCENE_DRIVER_H_
#define AVBASE_MEDIA_GL_GL_SCENE_DRIVER_H_

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "media/base/render_perf_stats.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"
#include "media/media_export.h"

namespace avbase::media {

// Keeps a GlScene in step with a stream of frames and presents it.
//
// This is the seam between the player's model -- one frame at a time, one sink
// -- and the compositor's, which wants a scene it can place, order and blend.
// The player has no second source today, so the HOST is what says what else is
// on screen: it installs an update callback and the driver hands it the scene
// once per frame.
//
// The main layer belongs to the driver: every frame the stream delivers is
// uploaded into it BEFORE the callback runs. So the common case -- one source
// in a scene -- needs no host code at all, and everything the callback adds
// (a picture-in-picture, a second source, a solid background) sits around a
// layer it does not have to create or feed.
//
// The callback is called once per displayed frame, not once per arriving
// frame: a dropped frame drops its callback with it, which is what keeps a
// scene's other layers in step with the video rather than with the clock.
//
// THREADING: Present() and Shutdown() must run on the thread holding the GL
// context -- the sink's render thread -- because the scene they create owns GL
// objects. The callback runs there too: it must not block and must not touch
// state the host's own thread is mutating. Installing the callback is the
// host's job before playback starts, the same lifecycle contract
// VideoRendererSink::Initialize() has.
class AVBASE_MEDIA_EXPORT GlSceneDriver {
 public:
  // |main_layer| is the layer the stream's frames are uploaded into.
  //
  // The presenter and the frame's timing come along so the host can feed its
  // own layers through GlPresenter::UploadLayer(), which is how a second
  // source's upload ends up in the same per-frame number as the first. The
  // frame arrives as a scoped_refptr rather than a plain reference because a
  // host that shows a delayed copy of the same stream is the normal case for a
  // compositor, and holding a reference is how it does that.
  using UpdateCallback = base::RepeatingCallback<void(
      GlPresenter& presenter, GlScene& scene, GlScene::LayerId main_layer,
      const base::scoped_refptr<VideoFrame>& frame, PresentTiming* timing)>;

  GlSceneDriver();
  ~GlSceneDriver();
  GlSceneDriver(const GlSceneDriver&) = delete;
  GlSceneDriver& operator=(const GlSceneDriver&) = delete;

  // Installs the host hook. Never calling it leaves Present() forwarding to
  // GlPresenter::Present(), i.e. exactly the single-frame path -- so a host
  // that wants none of this pays one branch and allocates no scene.
  void SetUpdateCallback(UpdateCallback callback);

  // Uploads |frame| into the scene's main layer, runs the callback, and
  // presents. Falls back to the single-frame path when no callback is set.
  // |presenter| must be initialised.
  bool Present(GlPresenter& presenter,
               const base::scoped_refptr<VideoFrame>& frame,
               PresentTiming* timing);

  // Drops the borrow on the presenter's scene. Call it before the presenter is
  // destroyed; the sinks do exactly that. It performs no GL calls -- the scene
  // belongs to the presenter, which destroys it -- so it is safe from any
  // thread, including the host's after the render thread has stopped.
  void Shutdown();

 private:
  // Borrowed from GlPresenter::EnsureScene(); the presenter owns it.
  GlScene* scene_{nullptr};
  GlScene::LayerId main_layer_{GlScene::kNoLayer};
  UpdateCallback update_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_SCENE_DRIVER_H_
