// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// docs/14 stage 3: ONE player, N sources.
//
// WHAT THIS REPLACES. Until this stage a second picture meant a second PLAYER:
// player.scene()->AddSource("cam", 10)->MakePublisherFactory(), handed to a
// second Player's Deps, whose own pipeline polled Render() on its own thread
// and dropped the frames in a mailbox. Two pipelines, two demuxers, two clocks
// and one loose end: the second pipeline had to present SOMEWHERE for its
// frames to be pulled at all, which is the only reason the mailbox existed.
//
// The replacement is that the player builds ONE pipeline with N demuxers and
// gives each secondary leg a sink that publishes straight into its layer. Same
// picture on screen, no second clock to reconcile.
//
// WHY IT NEEDS TESTS. Every hop in that sentence is invisible from outside:
// AddSource() returns a layer handle whether or not anything will ever feed
// it, and PrepareAsync() succeeds whether or not it derived the secondary sink
// factory. The observable consequence of a missing hop is "the second picture
// never appears", which in GL is a black layer indistinguishable from a file
// that failed to open. So each hop gets an assertion on the thing DOWNSTREAM
// of it, not on the setter that claims to have done it.

#include "player/player_impl.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "avbase/BuildConfig.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/simple_test_tick_clock.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/filters/null_video_sink.h"
#include "media/gl/gl_scene_engine.h"
#include "player/public/deps.h"
#include "player/public/player_config.h"
#include "player/public/scene.h"

#if !AVBASE_ENABLE_FFMPEG
// PrepareAsync reaches the factory-deps assembly only when a demuxer can be
// built, and without FFmpeg CreateDemuxer() has nothing to return
// (player/player_impl_prepare.cc): the player reports kNotImplemented before
// any secondary leg exists. Compiled to nothing rather than registered and
// skipped -- "skipped everywhere" reads like coverage this suite does not
// have. Same reasoning as player_deps_wiring_unittest.cc, whose seam these
// tests share.
#else

namespace avbase {

// The seam player/public/scene.h grants a test: the player is the only other
// caller of Scene::Bind(), and it needs a whole pipeline to do it. NOT in the
// anonymous namespace below -- friendship is granted to this exact name. The
// definition matches tests/unit/player/scene_unittest.cc exactly.
class SceneTestPeer {
 public:
  static void Bind(Scene& scene, media::GlSceneEngine* engine) {
    scene.Bind(engine);
  }
};

namespace {

// The scene engine the layers are plumbed into. It needs no GL context: none
// of this reaches a draw call, because what is on trial is which layer a frame
// lands in, not what it looks like there.
class PlayerMultiSourceTest : public ::testing::Test {
 protected:
  // Registers |secondary_uris| as secondary sources (source ids 1..N) and runs
  // PrepareAsync. The URI deliberately does not exist: the demuxer fails
  // later, on the media sequence, AFTER the factory deps were assembled --
  // what is on trial is whether the legs got BUILT, not whether playback
  // would have worked. ~PlayerImpl's bounded StopSync absorbs the failure.
  void Prepare(const std::vector<std::string>& secondary_uris) {
    PlayerConfig config;
    // A fake clock, so nothing here waits on wall time: the real tick clock is
    // not what any assertion below is about.
    auto deps = std::make_unique<Deps>();
    deps->tick_clock = clock_;
    impl_ = std::make_unique<PlayerImpl>(config, std::move(deps));
    ASSERT_TRUE(impl_->SetDataSource("/nonexistent/avbase-multisrc-main.mp4")
                    .has_value());
    int z = 10;
    for (const auto& uri : secondary_uris) {
      layers_.push_back(
          impl_->AddSource(media::DataSourceDescriptor::FromUri(uri),
                           "cam" + std::to_string(z), z));
      z += 10;
    }
    impl_->set_deps_observer_for_testing(base::BindRepeating(
        [](PlayerMultiSourceTest* self,
           const media::DefaultRendererFactory::Deps& deps) {
          self->deps_ = deps;
          ++self->prepare_count_;
        },
        base::Unretained(this)));
    ASSERT_TRUE(impl_->PrepareAsync().has_value());
  }

  // Prepares the SAME player a second time, adding no sources. Used to ask
  // what survived a Reset().
  void PrepareAgain() {
    ASSERT_TRUE(impl_->SetDataSource("/nonexistent/avbase-multisrc-main.mp4")
                    .has_value());
    ASSERT_TRUE(impl_->PrepareAsync().has_value());
  }

  // Plumbs every registered layer into |engine_|, which is what turns a layer
  // from "a handle" into "somewhere a frame can go". Until then Publish() is
  // REFUSED -- so every frame-count assertion below is also proof this ran.
  //
  // The pipeline is stopped first, and that is not tidiness: Scene::Bind() is
  // also called BY the pipeline (through Deps::scene_observer) with whatever
  // engine its sink reports, and pipeline->Start() is only POSTED by
  // PrepareAsync -- so that call lands after PrepareAsync returns and would
  // overwrite this one. Same ordering the player itself has to respect.
  void Plumb() {
    impl_->StopSync(base::Seconds(5));
    SceneTestPeer::Bind(*impl_->scene(), &engine_);
  }

  // Introduced by stage 3 (and kept by it), PublisherSink now lives only
  // inside the player library. It still needs a compositor above it, so the
  // test supplies the smallest one: every Render() hands over a fresh frame.
  // Nothing counts what the compositor kept, so one black frame is enough.
  class EndlessCompositor final
      : public media::VideoRendererSink::RenderCallback {
   public:
    base::scoped_refptr<media::VideoFrame>
    Render(base::TimeTicks /*deadline_min*/,
           base::TimeTicks /*deadline_max*/) override {
      return media::VideoFrame::CreateBlackFrame(
          media::VideoFormat::kI420, media::Size{64, 64}, media::Size{64, 64},
          media::Rational{1, 1}, base::TimeDelta(), base::TimeDelta(), 0);
    }
    void OnFrameSubmitFailure() override {}
  };

  // Drives one secondary leg until its layer has counted a publish, bounded so
  // a wiring mistake is a failed assertion instead of a hung suite.
  bool FeedUntilPublished(std::unique_ptr<media::VideoRendererSink> leg,
                          SceneSource& expect_in_layer) {
    EndlessCompositor compositor;
    leg->Initialize(&compositor);
    leg->Start();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool arrived = false;
    while (std::chrono::steady_clock::now() < deadline) {
      if (expect_in_layer.frames_published() > 0) {
        arrived = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    leg->Stop();
    return arrived;
  }

  media::DefaultRendererFactory::Deps deps_;
  // How many PrepareAsync runs reached the factory-deps assembly. Needed
  // because "no extra factory" is also what a prepare that never got there
  // looks like.
  int prepare_count_ = 0;
  std::vector<std::shared_ptr<SceneSource>> layers_;

  // The player under test, for assertions about registration itself rather
  // than about what it built.
  PlayerImpl& impl() { return *impl_; }

 private:
  // Outlives every assertion: the Scene owns the plumbing, so destroying the
  // player would unbind the layers mid-test.
  std::unique_ptr<PlayerImpl> impl_;
  // Shared, not a bare pointer: Deps::tick_clock owns its clock, so whoever
  // outlives the Deps keeps it.
  std::shared_ptr<base::SimpleTestTickClock> clock_ =
      std::make_shared<base::SimpleTestTickClock>();
  // DECLARED LAST ON PURPOSE, which means destroyed FIRST: the engine reports
  // its own death (GlSceneEngine::SetTeardownCallback) to whoever bound it,
  // so it has to outlive nobody. Reverse it -- engine after the player -- and
  // the engine's destructor calls Scene::OnEngineGone() on a Scene that is
  // already gone, which is a use-after-free wearing an unrelated error.
  media::GlSceneEngine engine_;
};

// ---------------------------------------------------------------------------
// The translation: registered sources become an extra sink factory.
// ---------------------------------------------------------------------------

TEST_F(PlayerMultiSourceTest, WithoutRegistrationThereIsNoExtraFactory) {
  // The single-source path has to stay EXACTLY what it was: no registration,
  // no secondary sink factory, nothing the renderer has to know about. This is
  // the mutation every other test here would miss -- the pessimised default,
  // where one source pays for machinery it can never use.
  Prepare({});
  EXPECT_EQ(deps_.extra_video_sink_factory, nullptr);
}

TEST_F(PlayerMultiSourceTest, ARegisteredSourceBecomesAnExtraSinkFactory) {
  Prepare({"/nonexistent/avbase-multisrc-cam.mp4"});
  ASSERT_EQ(layers_.size(), 1u);
  ASSERT_NE(layers_[0], nullptr);
  EXPECT_EQ(layers_[0]->name(), "cam10");
  EXPECT_EQ(layers_[0]->z(), 10);
  // The hop that would otherwise be silent: AddSource() succeeded, but nothing
  // downstream of it is a "second picture" unless PrepareAsync derived this.
  EXPECT_NE(deps_.extra_video_sink_factory, nullptr);
}

// ---------------------------------------------------------------------------
// The routing: each leg feeds ITS OWN layer.
// docs/14 §6.3 mutation M -- delete one source's AddSource wiring and the
// layer belonging to that source stops receiving frames.
// ---------------------------------------------------------------------------

TEST_F(PlayerMultiSourceTest, EachLegPublishesIntoItsOwnLayer) {
  Prepare({"/nonexistent/avbase-multisrc-cam1.mp4",
           "/nonexistent/avbase-multisrc-cam2.mp4"});
  ASSERT_EQ(layers_.size(), 2u);
  ASSERT_NE(layers_[0], nullptr);
  ASSERT_NE(layers_[1], nullptr);
  ASSERT_NE(deps_.extra_video_sink_factory, nullptr);
  Plumb();

  // Source ids are 1-based in this factory: source 0 is the player's own
  // SetDataSource() and takes the host sink, so it never reaches here.
  const bool arrived =
      FeedUntilPublished(deps_.extra_video_sink_factory->CreateForSource(
                             1, base::scoped_refptr<media::NativeDisplay>()),
                         *layers_[0]);
  EXPECT_GT(layers_[0]->frames_published(), 0u);
  ASSERT_TRUE(arrived) << "leg 1 published nothing into its own layer";

  // THE ROUTING ASSERTION. If every leg were wired to the same layer -- or to
  // whichever happened to be first -- this is the line that goes red, and
  // nothing above it would: the factory exists, a frame arrived, one layer
  // counted. Only "which one" separates correct from wrong.
  EXPECT_EQ(layers_[1]->frames_published(), 0u);

  // And leg 2 reaches layer 2, so this is not "the first one is special":
  // every source id maps to its own layer.
  const bool arrived2 =
      FeedUntilPublished(deps_.extra_video_sink_factory->CreateForSource(
                             2, base::scoped_refptr<media::NativeDisplay>()),
                         *layers_[1]);
  EXPECT_TRUE(arrived2) << "leg 2 published nothing into its own layer";
  EXPECT_GT(layers_[0]->frames_published(), 0u);
}

// ---------------------------------------------------------------------------
// Registration lifetime.
// ---------------------------------------------------------------------------

TEST_F(PlayerMultiSourceTest,
       ResetDropsRegistrationsSoReprepareIsSingleSource) {
  // Registrations belong to one playback, not to the player: Reset() clears
  // them, so a player that is reused for another file does not resurrect a
  // second demuxer nobody asked for -- which would fail to open and take the
  // whole prepare down with it.
  Prepare({"/nonexistent/avbase-multisrc-cam.mp4"});
  ASSERT_NE(deps_.extra_video_sink_factory, nullptr);
  impl().Reset();
  deps_ = media::DefaultRendererFactory::Deps();
  PrepareAgain();
  // Guards the assertion above against passing vacuously: if the second
  // PrepareAsync never reached the factory-deps assembly, "no extra factory"
  // would be true for a completely different reason.
  ASSERT_EQ(prepare_count_, 2);
  EXPECT_EQ(deps_.extra_video_sink_factory, nullptr);
}

TEST_F(PlayerMultiSourceTest, RegistrationIsRejectedOncePreparingStarted) {
  Prepare({});
  // A demuxer is built at PrepareAsync, so registering after it cannot take
  // effect -- which has to be a refusal rather than a registration nobody
  // honours, because "the layer exists but stays black" is exactly the failure
  // this whole file exists to give a name.
  EXPECT_EQ(impl().AddSource(media::DataSourceDescriptor::FromUri(
                                 "/nonexistent/avbase-multisrc-late.mp4"),
                             "late", 10),
            nullptr);
}

}  // namespace
}  // namespace avbase

#endif  // AVBASE_ENABLE_FFMPEG
