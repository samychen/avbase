// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_PUBLIC_SCENE_H_
#define AVBASE_PLAYER_PUBLIC_SCENE_H_

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "media/base/video_frame.h"
#include "player/public/player_export.h"

namespace avbase {

// The media-layer types the scene is built on. Named here, not included: none
// of them appears in a public signature, so an SDK caller never has to have
// media/gl in its include path to use a Scene.
namespace media {
class GlLayerSource;
class GlPresenter;
class GlScene;
class GlSceneEngine;
class VideoRendererSinkFactory;
struct PresentTiming;
}  // namespace media

class Scene;
class SceneFrame;
class SceneLayer;
class SceneSource;

// How a layer's picture is placed inside its rect.
enum class SceneContentMode {
  // Fills the rect exactly, ignoring the picture's aspect. What every layer
  // did before this enum existed, and still the default, so nothing moves.
  kStretch = 0,
  // Fits the WHOLE picture inside the rect, centred, leaving the scene's
  // background on whichever axis is longer: the letterbox a player puts
  // around a clip whose shape is not the window's.
  kFit,
};

// A layer in a scene: where the composition draws one picture.
//
// A HANDLE, not an owner. Copies are interchangeable and none of them keeps
// the layer alive -- removing it from the scene is what does that, and after
// that every copy answers "this names nothing" instead of quietly addressing
// a different layer, which is why ids are handed out and never reused.
//
// VALIDITY: only inside the callback it came from. That is enforced rather
// than merely documented -- see SceneFrame.
class AVBASE_PLAYER_EXPORT SceneLayer {
 public:
  SceneLayer() = default;

  bool valid() const;
  explicit operator bool() const { return valid(); }
  uint64_t id() const { return id_; }

  // Every setter here REJECTS a bad value instead of clamping it. A setter
  // can say no, so it does: NaN read as 0 makes a layer vanish for a reason
  // nothing records, and a rect reaching outside the picture would be sampled
  // with CLAMP_TO_EDGE and smear its edge row over the uncovered strip.

  // Where the picture lands, in NORMALISED composition coordinates: (0,0) is
  // the top-left of the whole composition, (1,1) the bottom-right, and w/h
  // are fractions of it. The default is everything.
  bool SetRect(float x, float y, float w, float h);
  // Draw order: lower z is drawn first, so higher z ends up on top. Ties keep
  // insertion order.
  bool SetZ(int z);
  // [0,1]. Below 1 the layer blends with what is already on the target.
  bool SetOpacity(float opacity);
  // A hidden layer is skipped entirely and keeps its picture, so hiding and
  // showing again costs nothing.
  bool SetVisible(bool visible);
  // Clockwise about the layer's own centre, in degrees. nullopt -- the
  // default -- follows the frame, which is where a container's display matrix
  // ends up; a value pins the angle and stops it following. Snapped to a
  // quarter turn, because that is the only set any stage here can carry.
  bool SetRotation(std::optional<int> degrees);
  // The part of this layer's OWN picture that is drawn, normalised over the
  // frame as the decoder stored it. Read here rather than in the turned
  // frame, so one rect means the same pixels whichever way the clip was shot.
  bool SetSourceRect(float x, float y, float w, float h);
  bool SetContentMode(SceneContentMode mode);

  // The angle this layer is drawn at, after the frame's own rotation and any
  // override have been combined.
  int rotation() const;

  // Replaces the picture this layer is showing. This is how you feed a layer
  // nothing is publishing into -- a logo, or a delayed copy of the main frame.
  // A layer WITH a source is fed by its own producer, not here.
  bool Upload(const media::VideoFrame& frame);

 private:
  friend class SceneFrame;
  SceneLayer(SceneFrame* frame, uint64_t id) : frame_(frame), id_(id) {}

  SceneFrame* frame_{nullptr};
  uint64_t id_{0};
};

// Once-per-displayed-frame access to the composition.
//
// Given to a SceneUpdateCallback, which runs on the render thread after every
// source has synced and before the scene is drawn -- so whatever the hook adds
// or moves is part of the frame that comes out.
//
// VALID ONLY FOR THE DURATION OF THE CALLBACK, and that is a guarantee rather
// than a warning: SceneLayer keeps a pointer back here, and this object is
// marked dead when the callback returns. A handle kept past the callback fails
// every call instead of reaching into a frame that has gone.
class AVBASE_PLAYER_EXPORT SceneFrame {
 public:
  SceneFrame() = default;
  SceneFrame(const SceneFrame&) = delete;
  SceneFrame& operator=(const SceneFrame&) = delete;

  // The layer the player's own stream went into this frame.
  SceneLayer main_layer();

  // Adds an empty layer at |z|, or returns an invalid handle when the scene
  // refused one. The layer stays until RemoveLayer().
  SceneLayer AddLayer(int z);
  bool RemoveLayer(SceneLayer layer);
  size_t layer_count() const;

  // The layer a registered source is drawing into, or an invalid handle when
  // that source has not been synced yet -- which is the first frame after
  // AddSource(). Placement has to wait: until the engine has made the layer
  // there is nothing to place.
  SceneLayer layer_of(const SceneSource& source);

  // The layer with this id, or an invalid handle when it is gone -- removed,
  // or belonging to a scene that has since been rebuilt after a Reset.
  //
  // How a hook remembers a layer it made on an earlier frame. Handles cannot
  // be kept, because the frame they were made from is dead by then; ids can,
  // because they are never reused. Asking the scene is what turns "an id I
  // wrote down" back into something usable.
  SceneLayer layer_with_id(uint64_t id);

 private:
  friend class Scene;
  friend class SceneLayer;

  void Begin(media::GlScene* scene, media::GlPresenter* presenter,
             media::PresentTiming* timing, uint64_t main_layer);
  void End();

  bool live() const { return scene_ != nullptr; }
  media::GlScene* scene() const { return scene_; }
  media::GlPresenter* presenter() const { return presenter_; }
  media::PresentTiming* timing() const { return timing_; }

  media::GlScene* scene_{nullptr};
  media::GlPresenter* presenter_{nullptr};
  media::PresentTiming* timing_{nullptr};
  uint64_t main_layer_{0};
};

// Called on the render thread once per displayed frame.
//
// What belongs here is PLACEMENT -- rect, opacity, z, and feeding any layer
// the player is not feeding. Registration does not: Scene::AddSource() is
// callable at any time, and doing it per frame would add the same source
// thousands of times.
using SceneUpdateCallback = base::RepeatingCallback<void(
    SceneFrame& frame,
    const base::scoped_refptr<media::VideoFrame>& main_frame)>;

// A second (third, ...) input into the composition: where a producer that is
// not this player pushes frames.
//
// The handle survives everything the scene does. Add it once, publish into it
// whenever, and it is re-plumbed automatically when the pipeline is rebuilt --
// which happens on every Prepare, because the sink belongs to the renderer and
// the renderer belongs to the pipeline. Hiding that churn is most of why this
// class exists.
class AVBASE_PLAYER_EXPORT SceneSource
    : public std::enable_shared_from_this<SceneSource> {
 public:
  const std::string& name() const { return name_; }
  int z() const { return z_; }

  // Hands over the newest frame. Any thread, any number of them.
  //
  // False when nothing is currently taking frames -- before the first Prepare,
  // or after a Stop -- in which case nothing was counted and nothing will be
  // shown. True does NOT mean displayed: a frame still in the slot when a
  // newer one arrives is superseded, which for a picture is the right thing
  // to drop. frames_superseded() is how a producer finds out.
  bool Publish(base::scoped_refptr<media::VideoFrame> frame);

  uint64_t frames_published() const;
  uint64_t frames_consumed() const;
  uint64_t frames_superseded() const;

 private:
  friend class Scene;
  friend class SceneFrame;
  SceneSource(std::string name, int z);

 public:
  // A video sink factory whose sinks publish whatever they render into this
  // source instead of displaying it. Hand it to a second Player's Deps and
  // that player's output becomes an input to this scene:
  //
  //   auto second = player.scene()->AddSource("camera", 10);
  //   deps->video_sink_factory = second->MakePublisherFactory();
  //
  // Needed because a second pipeline is a whole pipeline -- its own demuxer,
  // decoder and clock -- and it still has to present SOMEWHERE, because that
  // is the only way its frames get pulled. A sink that accepts the pull and
  // drops the frames in this source's mailbox is what "N independent decoders"
  // has to mean in a library whose renderer is single-source, and until this
  // existed the only way to build one was to hold a media::GlLayerSource and
  // write the sink out longhand.
  //
  // Frames published while nothing is plumbed in are dropped, which is what
  // starting the second pipeline before the first one has prepared has to mean.
  std::shared_ptr<media::VideoRendererSinkFactory> MakePublisherFactory();

 private:
  // Takes ownership of the engine-side source this one is plumbed into, or
  // clears it when there is no engine. Called by Scene, which is the only
  // object that knows when a pipeline started and stopped existing.
  void Bind(std::shared_ptr<media::GlLayerSource> source);
  // Drops it, folding what it counted into |carried_*| first so the totals
  // belong to this source rather than to whichever pipeline happened to be
  // running. A GlLayerSource's counters start at zero and die with it; a
  // producer's do not. The Locked form is for callers already holding |lock_|.
  void Retire();
  void RetireLocked() EXCLUSIVE_LOCKS_REQUIRED(lock_);

  const std::string name_;
  const int z_;

  mutable base::Lock lock_;
  std::shared_ptr<media::GlLayerSource> source_ GUARDED_BY(lock_);
  uint64_t carried_published_ GUARDED_BY(lock_){0};
  uint64_t carried_consumed_ GUARDED_BY(lock_){0};
  uint64_t carried_superseded_ GUARDED_BY(lock_){0};
};

// The player's composition surface: the one place several pictures become one.
//
// OWNED BY THE PLAYER and returned by Player::scene().
//
// A scene is a handle, not a promise. Whether it can currently DO anything
// depends on whether the running pipeline's sink can composite -- and the
// default headless sink cannot, so a zero-configuration Player has a scene
// that does nothing. That is answered in words by Describe() rather than
// discovered by calling something and watching it fail.
//
// THREAD SAFETY: every method may be called from any thread at any time. None
// blocks on the render thread and none needs GL to be current, which is what
// lets a UI thread move a layer while a frame is being drawn.
//
// LIFETIME: the scene outlives pipelines. The renderer is rebuilt every
// Prepare and takes its sink -- and therefore its engine -- with it, so a host
// that had to re-register everything each cycle would be writing lifecycle
// code it has no business writing. Sources and the update callback are
// re-plumbed into each new pipeline automatically.
class AVBASE_PLAYER_EXPORT Scene {
 public:
  Scene();
  Scene(const Scene&) = delete;
  Scene& operator=(const Scene&) = delete;
  ~Scene();

  // ---- Registration. Any thread, any time. ------------------------------

  // Names are not required to be unique: a name is how a host finds its own
  // source again, not an identity, and FindSource() answers with the first.
  std::shared_ptr<SceneSource> AddSource(std::string name, int z);
  bool RemoveSource(const std::shared_ptr<SceneSource>& source);
  std::shared_ptr<SceneSource> FindSource(std::string_view name) const;
  size_t source_count() const;

  // Installs the per-frame placement hook. Callable at any time; it takes
  // effect from the next frame and replaces whatever was there, because one
  // frame runs one hook.
  void SetUpdateCallback(SceneUpdateCallback callback);

  // The colour an uncovered part of the composition is cleared to, in [0,1]
  // RGBA. Opaque black by default. On the scene rather than on a layer:
  // "what is behind everything" has one answer for the whole composition.
  bool SetBackgroundColor(float r, float g, float b, float a);

  // ---- Diagnostics ------------------------------------------------------

  // One line saying whether this scene can do anything right now, and why not
  // when it cannot. Worth attaching to a bug report: "picture-in-picture is
  // not showing" has several quite different answers and this names which.
  std::string Describe() const;

 private:
  friend class PlayerImpl;
  // A TEST SEAM, deliberately narrower than the alternative. Bind() could have
  // been public, but then any caller could hand this object a pointer to
  // anything at all and call it an engine; this way the only other name that
  // can reach it is one a test defines, and a host never has an engine anyway.
  friend class SceneTestPeer;

  // Called by the player when it builds a renderer whose sink can composite,
  // with that sink's engine; and again with null before that engine can go
  // away. Private because nothing outside the player has an engine to hand us.
  void Bind(media::GlSceneEngine* engine);
  // Called from inside the engine's destructor, so it must not touch it.
  void OnEngineGone();

  // Plumbs one source into the bound engine, if there is one. With |lock_|
  // held. Every Bind() re-plumbs all of them: a new pipeline's engine knows
  // nothing about the old one's sources, so there is no state to carry over
  // beyond what this object already remembers.
  void PlumbLocked(SceneSource& source) EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // Installs or clears the trampoline on the bound engine. What decides it is
  // whether there is anything for a hook to DO: the engine's fast path -- no
  // sources and no hook means it hands the frame straight to the single-frame
  // presenter -- is worth preserving, so the trampoline must not be installed
  // "just in case". Called with |lock_| already held.
  void InstallHookLocked() EXCLUSIVE_LOCKS_REQUIRED(lock_);

  // The render-thread trampoline. From GlSceneEngine's point of view this is
  // the update hook; from ours it is the one place that knows how to turn a
  // GlScene into the SceneFrame the host's callback is handed -- and the one
  // place that can mark that frame dead when the callback returns.
  static void RunHook(Scene* self, media::GlPresenter& presenter,
                      media::GlScene& gl_scene, uint64_t main_layer,
                      const base::scoped_refptr<media::VideoFrame>& frame,
                      media::PresentTiming* timing);

  mutable base::Lock lock_;
  std::vector<std::shared_ptr<SceneSource>> sources_ GUARDED_BY(lock_);
  SceneUpdateCallback hook_ GUARDED_BY(lock_);
  std::array<float, 4> background_ GUARDED_BY(lock_){0.f, 0.f, 0.f, 1.f};
  bool background_set_ GUARDED_BY(lock_){false};
  media::GlSceneEngine* engine_ GUARDED_BY(lock_){nullptr};
};

}  // namespace avbase

#endif  // AVBASE_PLAYER_PUBLIC_SCENE_H_
