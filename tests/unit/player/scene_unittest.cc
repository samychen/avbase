// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The SDK's composition surface, checked without a GL context.
//
// WHAT THIS PINS DOWN. Until this existed, compositing was reachable only by
// injecting a VideoRendererSinkFactory and static_cast-ing the sink it handed
// back down to GlVideoSink -- which is undefined behaviour whenever the sink
// is not that class, and the default Deps produce a NullVideoSink, so it is
// undefined behaviour in the configuration every zero-setup host is in. The
// tests here are the shape of the replacement: a scene that is always
// reachable, and that says in words when it cannot currently do anything
// instead of letting a host discover it by watching a layer never appear.

#include "player/public/scene.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/video_frame.h"
#include "media/filters/null_video_sink.h"
#include "media/gl/gl_scene_engine.h"
#include "player/public/player.h"

namespace avbase {

// The seam player/public/scene.h grants a test: the player is the only other
// caller of Scene::Bind(), and it needs a whole pipeline to do it. NOT in the
// anonymous namespace below -- friendship is granted to this exact name.
class SceneTestPeer {
 public:
  static void Bind(Scene& scene, media::GlSceneEngine* engine) {
    scene.Bind(engine);
  }
};

namespace {

// A frame to publish. Content is irrelevant here: what is on trial is whether
// publishing is accepted, not what lands on screen.
base::scoped_refptr<media::VideoFrame> AnyFrame() {
  return media::VideoFrame::CreateBlackFrame(
      media::VideoFormat::kI420, media::Size{64, 64}, media::Size{64, 64},
      media::Rational{1, 1}, base::TimeDelta(), base::TimeDelta(), 0);
}

TEST(SceneTest, EveryPlayerHasASceneAndItIsNeverNull) {
  Player player;
  // Not conditional on a pipeline, a display or a backend: the scene is a
  // property of the player, and what varies is whether it can act.
  ASSERT_NE(player.scene(), nullptr);
}

TEST(SceneTest, DescribeNamesTheReasonWhenItCannotComposite) {
  Player player;
  // A default Deps carries no video sink factory, so the renderer gets a
  // NullVideoSink -- a real VideoRendererSink that has no scene. "The PiP is
  // not showing" then has a completely different cause from "the PiP is in
  // the wrong place", and this is what separates them.
  const std::string describe = player.scene()->Describe();
  EXPECT_NE(describe.find("unbound"), std::string::npos) << describe;
  EXPECT_NE(describe.find("cannot composite"), std::string::npos) << describe;
}

TEST(SceneTest, SourcesAreRegisteredBeforeAnyPipeline) {
  Player player;
  auto second = player.scene()->AddSource("second", /*z=*/10);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->name(), "second");
  EXPECT_EQ(second->z(), 10);
  EXPECT_EQ(player.scene()->source_count(), 1u);

  // Names are for finding a source again, not identity: two may share one and
  // the first wins.
  EXPECT_EQ(player.scene()->FindSource("second"), second);
  EXPECT_EQ(player.scene()->FindSource("nope"), nullptr);

  EXPECT_TRUE(player.scene()->RemoveSource(second));
  // A second removal is false rather than a no-op success: "was that mine" is
  // a question with one right answer.
  EXPECT_FALSE(player.scene()->RemoveSource(second));
  EXPECT_EQ(player.scene()->source_count(), 0u);
}

TEST(SceneTest, PublishingWithNothingPlumbedIsRefused) {
  // There is no engine to take the frame, so the honest answer is false and
  // nothing is counted. Reporting it as taken would leave "the second layer
  // is black" with no way to tell whether the producer or the pipeline is at
  // fault.
  Player player;
  auto second = player.scene()->AddSource("second", /*z=*/10);
  ASSERT_NE(second, nullptr);
  EXPECT_FALSE(second->Publish(AnyFrame()));
  EXPECT_EQ(second->frames_published(), 0u);
  EXPECT_EQ(second->frames_consumed(), 0u);
  EXPECT_EQ(second->frames_superseded(), 0u);
}

TEST(SceneTest, BackgroundColourRejectsANonFiniteValue) {
  Player player;
  // Rejected rather than clamped, like every other setter here: reading a NaN
  // as 0 makes a layer vanish for a reason nothing records.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(player.scene()->SetBackgroundColor(nan, 0.0f, 0.0f, 1.0f));
  EXPECT_FALSE(player.scene()->SetBackgroundColor(0.0f, nan, 0.0f, 1.0f));
  EXPECT_TRUE(player.scene()->SetBackgroundColor(0.0f, 0.0f, 0.0f, 1.0f));
}

TEST(SceneTest, ALayerHandleNobodyHandedOutIsInvalid) {
  // The default-constructed handle, and what a hook gets when it asks for a
  // layer that does not exist. Every setter on it has to fail rather than
  // address a layer it was never given.
  SceneLayer layer;
  EXPECT_FALSE(layer.valid());
  EXPECT_FALSE(static_cast<bool>(layer));
  EXPECT_FALSE(layer.SetRect(0.0f, 0.0f, 1.0f, 1.0f));
  EXPECT_FALSE(layer.SetZ(1));
  EXPECT_FALSE(layer.SetOpacity(0.5f));
  EXPECT_FALSE(layer.SetVisible(false));
  EXPECT_FALSE(layer.SetRotation(90));
  EXPECT_FALSE(layer.SetSourceRect(0.0f, 0.0f, 1.0f, 1.0f));
  EXPECT_FALSE(layer.SetContentMode(SceneContentMode::kFit));
  EXPECT_FALSE(layer.Upload(*AnyFrame()));
  EXPECT_EQ(layer.rotation(), 0);
}

// A sink that cannot composite has to be able to SAY so, because it is asked
// and never inferred. RTTI is off project-wide, so there is no dynamic_cast to
// fall back on -- which is why this is a virtual and not a discovery.
TEST(SceneTest, ASinkThatCannotCompositeSaysSo) {
  media::NullVideoSink sink;
  sink.SetOutputTarget(nullptr);
  EXPECT_EQ(sink.scene_engine(), nullptr)
      << "a null sink is a real VideoRendererSink with no scene to hand out";
}

// Binding a pipeline is what turns registration into something that works, and
// unbinding is what stops it -- with the source's own counts surviving both.
TEST(SceneTest, BindingAPipelineIsWhatMakesPublishingWork) {
  Player player;
  auto second = player.scene()->AddSource("second", /*z=*/10);
  ASSERT_NE(second, nullptr);
  // Registered, but plumbed nowhere: no pipeline has been built yet, so there
  // is no engine-side slot for this frame to land in.
  EXPECT_FALSE(second->Publish(AnyFrame()));

  media::GlSceneEngine engine;
  SceneTestPeer::Bind(*player.scene(), &engine);
  EXPECT_NE(player.scene()->Describe().find("bound"), std::string::npos)
      << player.scene()->Describe();
  EXPECT_TRUE(second->Publish(AnyFrame()));
  EXPECT_EQ(second->frames_published(), 1u);

  SceneTestPeer::Bind(*player.scene(), nullptr);
  EXPECT_FALSE(second->Publish(AnyFrame()));
  // Carried rather than reset: how much this producer has fed is a fact about
  // the producer, not about whichever pipeline happens to be running.
  EXPECT_EQ(second->frames_published(), 1u);
}

// The engine reports its own death, and that is the whole reason a scene can
// hold a pointer at it without one more thing to remember.
//
// The player does unbind when it stops or resets on purpose, but a pipeline is
// also torn down by its own error handling, and an observer that waits to be
// told by the player is waiting for something that may never happen while
// still believing the pointer is good.
TEST(SceneTest, TheEngineReportsItsOwnDeath) {
  Player player;
  auto second = player.scene()->AddSource("second", /*z=*/10);
  ASSERT_NE(second, nullptr);
  {
    media::GlSceneEngine engine;
    SceneTestPeer::Bind(*player.scene(), &engine);
    EXPECT_TRUE(second->Publish(AnyFrame()));
  }
  // Nobody called Bind(nullptr) here -- the engine's destructor did.
  EXPECT_NE(player.scene()->Describe().find("unbound"), std::string::npos)
      << player.scene()->Describe();
  EXPECT_FALSE(second->Publish(AnyFrame()));
  EXPECT_EQ(second->frames_published(), 1u);
}

TEST(SceneTest, TheUpdateCallbackIsInstallableAtAnyTime) {
  // It used to be installable only before the sink started, because the engine
  // wrote the callback from the host's thread and read it from the render
  // thread with no synchronisation at all. "Before Start" was a data race
  // wearing a contract's clothes; now it is just installable.
  Player player;
  int runs = 0;
  player.scene()->SetUpdateCallback(base::BindRepeating(
      [](int* out, SceneFrame&, const base::scoped_refptr<media::VideoFrame>&) {
        ++*out;
      },
      &runs));
  // Nothing runs it until there is a pipeline, and that is not a defect --
  // but installing it must be legal and must not have required one.
  EXPECT_EQ(runs, 0);
  // Replacing it is legal too, and one frame runs one hook.
  player.scene()->SetUpdateCallback(SceneUpdateCallback());
  EXPECT_EQ(runs, 0);
}

}  // namespace
}  // namespace avbase
