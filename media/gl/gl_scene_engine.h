// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_GL_GL_SCENE_ENGINE_H_
#define AVBASE_MEDIA_GL_GL_SCENE_ENGINE_H_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "media/base/render_perf_stats.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_layer_source.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"
#include "media/media_export.h"

namespace avbase::media {

// Orchestrates N sources into one scene and presents it.
//
// This is the seam between the player's model -- one stream, one sink, one
// frame at a time -- and a compositor's: several streams that arrive whenever
// they like and are placed wherever the application says. The sink PULLS its
// own frame on the display's clock and hands it here; every other source
// PUSHES into a GlLayerSource of its own, and this class is what meets the two
// once per displayed frame.
//
// WHY THE TWO DIRECTIONS ARE BOTH HERE. The asymmetry is real and not a
// wart: the player's own stream is paced by the sink's Render() because it is
// the stream A/V sync is measured against, while a second source has its own
// clock and cannot be asked for a frame at a deadline it has never heard of.
// Forcing both through one shape would mean either blocking the display on a
// foreign decoder or buffering the player's frame through a mailbox that
// exists only to satisfy an abstraction.
//
// ONE SOURCE IS A CASE, NOT A BRANCH IN THE CALLER. With no sources added and
// no host callback, Present() is GlPresenter::Present() -- the single-frame
// path, unchanged. So the multi-source path is not a mode anybody has to
// enable, and a host that wants none of this pays one comparison.
//
// THREADING: AddSource()/RemoveSource()/FindSource()/source_count() from any
// thread; Publish() on a GlLayerSource from any thread. Present() and
// Shutdown() on the thread holding the GL context.
class AVBASE_MEDIA_EXPORT GlSceneEngine {
 public:
  // The host hook, run once per displayed frame after every source has synced
  // and before the scene is presented -- so what it adds (a background, a
  // logo, a layer it feeds itself) is composed with the frame that arrived.
  // |main_layer| is the layer the sink's own frame was uploaded into.
  using UpdateCallback = base::RepeatingCallback<void(
      GlPresenter& presenter, GlScene& scene, GlScene::LayerId main_layer,
      const base::scoped_refptr<VideoFrame>& frame, PresentTiming* timing)>;

  GlSceneEngine();
  GlSceneEngine(const GlSceneEngine&) = delete;
  GlSceneEngine& operator=(const GlSceneEngine&) = delete;
  ~GlSceneEngine();

  // Adds a source whose layer sits at |z|. The returned handle is what the
  // producer publishes into and stays valid as long as this engine or the
  // caller holds it -- shared because a source can outlive its own removal
  // below by the length of one frame.
  //
  // Names are not required to be unique: two sources may both be called
  // "camera" and FindSource() answers with the first, because a name is for
  // logs and for a host looking its own source up again, not an identity.
  std::shared_ptr<GlLayerSource> AddSource(std::string name, int z);

  // Retires a source. It stops being synced at once, but the layer it owns is
  // removed from the scene on the GL thread by the next Present() -- the
  // decision can be made anywhere, the GL object may not.
  //
  // False when |source| is not one of this engine's.
  bool RemoveSource(const std::shared_ptr<GlLayerSource>& source);

  std::shared_ptr<GlLayerSource> FindSource(std::string_view name) const;
  size_t source_count() const;

  // Installs the host hook. Never calling it leaves Present() forwarding to
  // the single-frame path when there are no sources either.
  void SetUpdateCallback(UpdateCallback callback);

  // Uploads |main_frame| into the scene's main layer, syncs every source, runs
  // the hook, and composes into the canvas. Everything except the blit and the
  // swap. |main_frame| must not be null; a sink that got no frame from Render()
  // decides for itself what that means (see GlVideoSink::PresentOne) instead of
  // asking here.
  //
  // The two stages are split for the same reason GlPresenter splits Compose()
  // from PresentComposed(): a test that wants to look at the composed pixels
  // has to read them BEFORE SwapBuffers() makes the back buffer undefined, and
  // the same seam is what a host capturing the composition would use.
  bool Compose(GlPresenter& presenter,
               const base::scoped_refptr<VideoFrame>& main_frame,
               PresentTiming* timing = nullptr);

  // Compose() + the blit + the swap: the whole frame, and the one call a sink
  // makes.
  bool Present(GlPresenter& presenter,
               const base::scoped_refptr<VideoFrame>& main_frame,
               PresentTiming* timing = nullptr);

  // Drops the borrow on the presenter's scene and detaches every source. It
  // performs no GL calls -- the scene belongs to the presenter, which
  // destroys it -- so it is safe from any thread, including the host's after
  // the render thread has stopped.
  void Shutdown();

 private:
  // The shared first half of both entry points: retire on the GL thread, make
  // sure the scene and the main layer exist, upload the main frame, sync every
  // source, run the hook. False on failure; otherwise |*scene| is the scene now
  // ready to compose, or null when there was nothing to orchestrate and the
  // caller should take the single-frame path itself.
  //
  // It stops after the hook rather than going on to compose, because that is
  // the only difference between Compose() and Present().
  bool Sync(GlPresenter& presenter,
            const base::scoped_refptr<VideoFrame>& main_frame,
            PresentTiming* timing, GlScene** scene);

  mutable base::Lock lock_;
  std::vector<std::shared_ptr<GlLayerSource>> sources_ GUARDED_BY(lock_);
  // Removed sources waiting for the GL thread to take their layer out of the
  // scene. Kept until then, because releasing the frame they hold is what
  // frees the surface an imported layer is sampling.
  std::vector<std::shared_ptr<GlLayerSource>> retiring_ GUARDED_BY(lock_);

  // Scratch for Present(): the source list is copied into it under |lock_| and
  // walked outside, so the GL thread never holds the lock across an upload.
  // Members rather than locals so a steady state does not allocate once per
  // displayed frame. GL thread only.
  std::vector<std::shared_ptr<GlLayerSource>> sync_list_;
  std::vector<std::shared_ptr<GlLayerSource>> retire_list_;

  // Borrowed from GlPresenter::EnsureScene(); the presenter owns it.
  GlScene* scene_{nullptr};
  GlScene::LayerId main_layer_{GlScene::kNoLayer};
  UpdateCallback update_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_SCENE_ENGINE_H_
