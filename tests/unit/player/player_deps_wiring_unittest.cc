// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The PlayerConfig -> DefaultRendererFactory::Deps translation, pinned down.
//
// WHY THIS EXISTS. Nothing in the tree observed what
// PrepareAsync hands the renderer factory. The consequence was concrete,
// not theoretical: wiring "max-fps" in round 71, an edit accidentally
// deleted `factory_deps.video_disabled = config_.video.disabled;` (the
// "vn" option) and all 528 tests stayed green -- the loss surfaced only in
// a pre-commit diff self-review. The seam these tests use
// (PlayerImpl::set_deps_observer_for_testing) exists so that hop can no
// longer change silently.
//
// WHAT IS NOT CLAIMED. The observer fires inside PrepareAsync, before
// pipeline->Start(); these tests see the Deps AS ASSEMBLED, not what the
// pipeline later does with them. The Deps -> RendererImpl hop is covered by
// tests/unit/media_filters/renderer_option_wiring_unittest.cc, whose header
// records that boundary honestly rather than pretending to reach here.

#include "player/player_impl.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "avbase/BuildConfig.h"
#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/simple_test_tick_clock.h"
#include "gtest/gtest.h"
#include "media/base/video_renderer_sink.h"
#include "media/filters/legacy/av_sync_controller.h"
#include "media/filters/null_video_sink.h"
#include "player/public/deps.h"
#include "player/public/player_config.h"

#if !AVBASE_ENABLE_FFMPEG
// PrepareAsync reaches the factory-deps assembly only when a demuxer can be
// built, and without FFmpeg CreateDemuxer() has nothing to return
// (player/player_impl_prepare.cc): the player reports kNotImplemented
// before the translation happens. So in that gate there is nothing to
// observe, and this translation unit compiles to nothing rather than
// registering tests that would have to skip -- "skipped everywhere" reads
// like coverage this suite does not have.
#else

namespace avbase {
namespace {

// What the seam captured. A copy, not a pointer: the Deps are moved into
// the factory the moment the observer returns, and a test that kept the
// address would be reading whatever the renderers did to them afterwards.
struct Captured {
  media::DefaultRendererFactory::Deps deps;
  bool seen = false;
  int fires = 0;
};

// Drives a real PlayerImpl through PrepareAsync and returns what it
// assembled. The URI is deliberately unopenable: the demuxer fails later,
// on the media sequence, AFTER the observer has fired -- what is on trial
// is the translation, not the playback. ~PlayerImpl's bounded StopSync
// absorbs the failure, and no event handler is installed because nobody
// is listening for one.
Captured PrepareAndCapture(const PlayerConfig& config,
                           std::unique_ptr<Deps> deps) {
  Captured captured;
  PlayerImpl impl(config, std::move(deps));
  // A path no machine has: the demuxer's Initialize() fails on the media
  // sequence, which is the point -- the failure is downstream of the hop
  // under test.
  const auto source =
      impl.SetDataSource("/nonexistent/avbase-deps-wiring-test.mp4");
  EXPECT_TRUE(source.has_value());
  impl.set_deps_observer_for_testing(base::BindRepeating(
      [](Captured* captured, const media::DefaultRendererFactory::Deps& deps) {
        captured->deps = deps;
        captured->seen = true;
        ++captured->fires;
      },
      base::Unretained(&captured)));
  const auto prepare = impl.PrepareAsync();
  EXPECT_TRUE(prepare.has_value());
  EXPECT_TRUE(captured.seen);
  EXPECT_EQ(captured.fires, 1);
  return captured;
}

// Minimal stand-in for a host's video sink factory: it counts its calls and
// hands back the headless sink, so the pointer-forwarding assertion below is
// backed by "the renderer was really built through this factory".
//
// An earlier version of this fake returned nullptr on the theory that "the
// pipeline fails at the demuxer before any sink exists". That theory is
// wrong and the crash it caused is the record: DoStart creates the RENDERER
// first and initializes the demuxer second, so the factory is always called
// on the way to the failure. NullVideoSink is what the default Deps fall
// back to -- it renders nowhere, opens no window, starts no cadence thread
// until Initialize().
class FakeSinkFactory final : public media::VideoRendererSinkFactory {
 public:
  std::unique_ptr<media::VideoRendererSink>
  Create(base::scoped_refptr<NativeDisplay> /*display*/) override {
    ++creates;
    return std::make_unique<media::NullVideoSink>();
  }
  const char* name() const override { return "fake"; }

  std::atomic<int> creates{0};
};

TEST(PlayerDepsWiringTest, DefaultConfigReachesTheFactory) {
  const Captured c = PrepareAndCapture(PlayerConfig(), nullptr);
  // The PlayerConfig defaults are what turn the cap ON: max-fps is 31
  // there and 0 (= no cap) in the factory's own Deps. Pinned because round
  // 71 made that a deliberate behaviour change -- a player now caps at 31
  // where a hand-built renderer would not -- and a change that silent
  // deserves a test.
  EXPECT_EQ(c.deps.video_max_fps, 31);
  EXPECT_EQ(c.deps.video_max_frame_drop, 0);
  EXPECT_FALSE(c.deps.video_disabled);
  EXPECT_FALSE(c.deps.audio_disabled);
  EXPECT_EQ(c.deps.video_decoder_preference, DecoderPreference::kAuto);
  EXPECT_TRUE(c.deps.video_decoder_preference_set);
  EXPECT_EQ(c.deps.video_hw_codecs,
            static_cast<HwCodecMask>(HwCodecFlag::kAll));
  EXPECT_TRUE(c.deps.video_filter_graph.empty());
  EXPECT_TRUE(c.deps.audio_filter_graph.empty());
  // The scene hook is installed unconditionally: a sink that can composite
  // has somewhere to report its engine even when the host never touched
  // the scene (docs/TODO §2).
  EXPECT_TRUE(c.deps.scene_observer);
  ASSERT_TRUE(c.deps.av_sync);
  // Asserted through the controller the facade builds, not through
  // deps.sync_master: the facade never sets that field, so through a Player
  // it stays at its default whatever "sync" says -- a host that constructs
  // DefaultRendererFactory directly is the only reader that field ever had.
  EXPECT_EQ(c.deps.av_sync->requested_master(),
            media::AvSyncController::MasterType::kAudio);
}

TEST(PlayerDepsWiringTest, VideoDisabledReachesTheFactory) {
  // The round-71 regression, as a test: this exact line was deleted by an
  // edit while wiring "max-fps", and 528 tests stayed green.
  PlayerConfig config;
  config.video.disabled = true;
  const Captured c = PrepareAndCapture(config, nullptr);
  EXPECT_TRUE(c.deps.video_disabled);
  // The other leg is untouched by this one: "vn" must not imply "an".
  EXPECT_FALSE(c.deps.audio_disabled);
}

TEST(PlayerDepsWiringTest, AudioDisabledReachesTheFactory) {
  PlayerConfig config;
  config.audio.disabled = true;
  const Captured c = PrepareAndCapture(config, nullptr);
  EXPECT_TRUE(c.deps.audio_disabled);
  EXPECT_FALSE(c.deps.video_disabled);
}

TEST(PlayerDepsWiringTest, PacingOptionsReachTheFactory) {
  PlayerConfig config;
  config.video.max_fps = 7;
  config.video.max_frame_drop = 3;
  const Captured c = PrepareAndCapture(config, nullptr);
  EXPECT_EQ(c.deps.video_max_fps, 7);
  EXPECT_EQ(c.deps.video_max_frame_drop, 3);
  // An explicit 0 (no cap) cannot be told apart from an unwired option --
  // the factory's own default is also 0 -- so the pair above is the half
  // that can distinguish. This half documents the opt-out only.
  PlayerConfig uncapped;
  uncapped.video.max_fps = 0;
  const Captured u = PrepareAndCapture(uncapped, nullptr);
  EXPECT_EQ(u.deps.video_max_fps, 0);
}

TEST(PlayerDepsWiringTest, DecoderPreferenceReachesTheFactory) {
  // docs/12 §2.3: before round 71 this field was honoured nowhere at
  // runtime, and kHardwareOnly silently fell back to software.
  PlayerConfig config;
  config.video.decoder_preference = DecoderPreference::kHardwareOnly;
  const Captured c = PrepareAndCapture(config, nullptr);
  EXPECT_EQ(c.deps.video_decoder_preference, DecoderPreference::kHardwareOnly);
  // The flag is what makes "unset" mean "the injected factory order is
  // authoritative" (default_renderer_factory.h); a player always sets it.
  EXPECT_TRUE(c.deps.video_decoder_preference_set);
}

TEST(PlayerDepsWiringTest, HwCodecMaskReachesTheFactory) {
  PlayerConfig config;
  config.video.hw_codecs = static_cast<HwCodecMask>(HwCodecFlag::kHevc);
  const Captured c = PrepareAndCapture(config, nullptr);
  EXPECT_EQ(c.deps.video_hw_codecs,
            static_cast<HwCodecMask>(HwCodecFlag::kHevc));
}

TEST(PlayerDepsWiringTest, FilterGraphsReachTheFactory) {
  PlayerConfig config;
  config.video.filter_graph = "scale=640:360";
  config.audio.filter_graph = "volume=0.5";
  const Captured c = PrepareAndCapture(config, nullptr);
  EXPECT_EQ(c.deps.video_filter_graph, "scale=640:360");
  EXPECT_EQ(c.deps.audio_filter_graph, "volume=0.5");
  // The stage factories ride with the FFmpeg build. Non-null is the wired
  // half; what the renderer does when a graph meets a hardware frame is
  // docs/TODO §3 -- a named refusal, not a silent downgrade.
  EXPECT_TRUE(c.deps.video_filter_factory);
  EXPECT_TRUE(c.deps.audio_filter_factory);
}

TEST(PlayerDepsWiringTest, SyncMasterMappingReachesTheFactory) {
  // ToMediaMaster is a three-way mapping with a silent fallback; pin all
  // three rather than the default every other test happens to exercise.
  const auto captured_for = [](SyncMasterType configured) {
    PlayerConfig config;
    config.sync_master = configured;
    return PrepareAndCapture(config, nullptr);
  };
  const Captured audio = captured_for(SyncMasterType::kAudio);
  const Captured video = captured_for(SyncMasterType::kVideo);
  const Captured external = captured_for(SyncMasterType::kExternal);
  ASSERT_TRUE(audio.deps.av_sync);
  ASSERT_TRUE(video.deps.av_sync);
  ASSERT_TRUE(external.deps.av_sync);
  EXPECT_EQ(audio.deps.av_sync->requested_master(),
            media::AvSyncController::MasterType::kAudio);
  EXPECT_EQ(video.deps.av_sync->requested_master(),
            media::AvSyncController::MasterType::kVideo);
  EXPECT_EQ(external.deps.av_sync->requested_master(),
            media::AvSyncController::MasterType::kExternal);
}

TEST(PlayerDepsWiringTest, InjectedClockReachesTheFactory) {
  // Held in a second shared_ptr on purpose: PlayerImpl owns the Deps, so
  // the clock has to outlive the player for the pointer comparison below
  // to compare anything at all.
  const auto clock = std::make_shared<base::SimpleTestTickClock>();
  auto deps = std::make_unique<Deps>();
  deps->tick_clock = clock;
  const Captured c = PrepareAndCapture(PlayerConfig(), std::move(deps));
  // The facade forwards the host's clock rather than substituting its own
  // default; a host with its own time source must be the one the renderers
  // see. (The constructor only fills a clock when none was injected -- the
  // "a hand-built Deps may not" branch in player_impl.cc.)
  EXPECT_EQ(c.deps.tick_clock, clock.get());
}

TEST(PlayerDepsWiringTest, InjectedSinkFactoryReachesTheFactory) {
  auto deps = std::make_unique<Deps>();
  const auto sink_factory = std::make_shared<FakeSinkFactory>();
  deps->video_sink_factory = sink_factory;
  const Captured c = PrepareAndCapture(PlayerConfig(), std::move(deps));
  // What docs/TODO §2's scene plumbing and every zero-configuration host
  // depend on: the host's factory is forwarded, not replaced by the null
  // one the default Deps would otherwise produce.
  EXPECT_EQ(c.deps.video_sink_factory.get(), sink_factory.get());
  // And it is the factory that really builds the sink: the pipeline creates
  // the renderer before the demuxer initializes, so the injected factory is
  // called exactly once on the way to the (unavoidable) failure.
  EXPECT_EQ(sink_factory->creates.load(), 1);
}

TEST(PlayerDepsWiringTest, NoDispReachesTheFactory) {
  // "nodisp" is not "vn": the stream still decodes and the video sink still
  // paces the clock, only the picture is dropped. So it travels on its own
  // field, and setting it must leave the vn/an pair alone -- the same mistake
  // that made the round-71 regression invisible would look identical here if
  // this test only checked the one flag.
  PlayerConfig config;
  config.render.disable_video_output = true;
  const Captured c = PrepareAndCapture(config, nullptr);
  EXPECT_TRUE(c.deps.disable_video_output);
  EXPECT_FALSE(c.deps.video_disabled);
  EXPECT_FALSE(c.deps.audio_disabled);
  // What the factory does with the flag is asserted where it happens
  // (default_renderer_factory_unittest.cc): this hop only carries it.
}

TEST(PlayerDepsWiringTest, DecoderFallbackChainReachesTheFactory) {
  // A hand-built Deps starts with no factories; the constructor appends
  // the FFmpeg tail, and the platform's hardware head when the preference
  // allows it. The COUNT is platform-dependent (macOS slots VideoToolbox
  // in, Linux without VAAPI does not), so this pins "non-empty" -- the
  // part that was silently droppable.
  const Captured c =
      PrepareAndCapture(PlayerConfig(), std::make_unique<Deps>());
  EXPECT_FALSE(c.deps.video_decoder_factories.empty());
  EXPECT_FALSE(c.deps.audio_decoder_factories.empty());
  EXPECT_TRUE(c.deps.text_decoder_factory);
}

}  // namespace
}  // namespace avbase

#endif  // !AVBASE_ENABLE_FFMPEG
