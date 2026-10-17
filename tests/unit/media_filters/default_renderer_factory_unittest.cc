// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Who picks the video sink, and what "nodisp" actually does to it.
//
// WHY THIS FILE EXISTS. config.render.disable_video_output ("nodisp") was a
// dead option for several rounds: the field existed, docs/10 §7 and the
// SinkNotAttached hint both promised "audio only playback, set this", and no
// sink selection read it, so a GL host that set it still got SinkNotAttached
// from a sink it never attached.
// DefaultRendererFactory::CreateVideoRendererSink is the one place a sink is
// chosen, so the fix lives there and the assertions live here.
//
// The two halves that matter are asserted separately, because either alone is
// satisfiable without the feature working:
//   * the injected factory is BYPASSED (otherwise a host backend still opens
//     the window the config asked not to have), and
//   * the sink it hands back really discards (otherwise the frames are counted
//     as presented while nothing is on screen, and every statistic downstream
//     -- including the ones this project's tests are built on -- lies).
//
// The second half is why the discard case passes a VALID display on purpose: a
// NullVideoSink told about a valid target reports frames as shown, so
// forwarding the display would flip the assertion. Passing nullptr would make
// the test agree with a broken implementation for the wrong reason.

#include "media/renderers/default_renderer_factory.h"

#include <memory>
#include <utility>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/native_display.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/filters/null_video_sink.h"

namespace avbase::media {
namespace {

// A stand-in for a host backend (sdl2/glfw/egl). It counts Create() calls so
// "the config overrode the host" is observable, and hands back a NullVideoSink
// so a test that accidentally starts it opens no window.
class CountingSinkFactory final : public VideoRendererSinkFactory {
 public:
  std::unique_ptr<VideoRendererSink>
  Create(base::scoped_refptr<NativeDisplay> /*display*/) override {
    ++creates;
    return std::make_unique<NullVideoSink>();
  }
  const char* name() const override { return "counting"; }

  int creates = 0;
};

// Hands the sink the same frame every tick. Non-null on purpose: a null frame
// is classified as a drop whether or not the sink was told to discard, so it
// could not tell the two apart.
class AlwaysFrameCallback final : public VideoRendererSink::RenderCallback {
 public:
  explicit AlwaysFrameCallback(base::scoped_refptr<VideoFrame> frame)
      : frame_(std::move(frame)) {}

  base::scoped_refptr<VideoFrame>
  Render(base::TimeTicks /*deadline_min*/,
         base::TimeTicks /*deadline_max*/) override {
    return frame_;
  }
  void OnFrameSubmitFailure() override {}

 private:
  base::scoped_refptr<VideoFrame> frame_;
};

class DefaultRendererFactoryTest : public ::testing::Test {
 protected:
  DefaultRendererFactoryTest()
      : env_(base::test::TaskEnvironment::TimeSource::kRealTime),
        video_thread_("avbase-test-S3"),
        audio_thread_("avbase-test-S4") {}

  void SetUp() override {
    ASSERT_TRUE(video_thread_.Start());
    ASSERT_TRUE(audio_thread_.Start());
  }

  void TearDown() override {
    video_thread_.Stop();
    audio_thread_.Stop();
  }

  // The three fields the factory CHECKs on. Everything else is left at the
  // factory's own defaults: this file is about sink selection, not about the
  // renderer.
  DefaultRendererFactory::Deps MakeDeps() {
    DefaultRendererFactory::Deps deps;
    deps.video_task_runner = video_thread_.task_runner();
    deps.audio_task_runner = audio_thread_.task_runner();
    deps.tick_clock = env_.GetTickClock();
    return deps;
  }

  // A display that reports valid() -- FromSdl2Window accepts a null window and
  // still tags the kind, which is all discard_ keys on.
  static base::scoped_refptr<NativeDisplay> ValidDisplay() {
    return NativeDisplay::FromSdl2Window(nullptr);
  }

  base::test::TaskEnvironment env_;
  base::Thread video_thread_;
  base::Thread audio_thread_;
};

TEST_F(DefaultRendererFactoryTest, InjectedFactoryBuildsTheSinkByDefault) {
  // The control case. Without it the nodisp assertions below could pass on a
  // factory that never calls the injected factory at all.
  auto counting = std::make_shared<CountingSinkFactory>();
  DefaultRendererFactory::Deps deps = MakeDeps();
  deps.video_sink_factory = counting;
  DefaultRendererFactory factory(std::move(deps));

  const auto sink = factory.CreateVideoRendererSink(ValidDisplay());
  ASSERT_TRUE(sink);
  EXPECT_EQ(counting->creates, 1);
}

TEST_F(DefaultRendererFactoryTest, NoDispBypassesTheInjectedFactory) {
  auto counting = std::make_shared<CountingSinkFactory>();
  DefaultRendererFactory::Deps deps = MakeDeps();
  deps.video_sink_factory = counting;
  deps.disable_video_output = true;
  DefaultRendererFactory factory(std::move(deps));

  const auto sink = factory.CreateVideoRendererSink(ValidDisplay());
  ASSERT_TRUE(sink);
  // The config outranks the host: this is the difference between "the app
  // asked for audio only" and "the app happened to pass a backend anyway".
  EXPECT_EQ(counting->creates, 0)
      << "nodisp must not let an injected backend open a window";
  EXPECT_STREQ(sink->name(), "NullVideoSink");
}

TEST_F(DefaultRendererFactoryTest, NoDispSinkDiscardsEvenWithAValidTarget) {
  DefaultRendererFactory::Deps deps = MakeDeps();
  deps.disable_video_output = true;
  DefaultRendererFactory factory(std::move(deps));

  const auto sink = factory.CreateVideoRendererSink(ValidDisplay());
  ASSERT_TRUE(sink);

  // 16x16 so the allocation is trivial; the sink never reads a pixel.
  const base::scoped_refptr<VideoFrame> frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size(16, 16), Size(16, 16), Rational(1, 1),
      base::TimeDelta(), base::Milliseconds(16), /*serial=*/1);
  ASSERT_TRUE(frame);
  AlwaysFrameCallback callback(frame);
  sink->Initialize(&callback);
  sink->Play();   // RenderOne() only counts while playing.
  sink->Start();  // Runs one tick inline before returning.
  const VideoSinkStats stats = sink->GetStats();
  sink->Stop();

  EXPECT_EQ(stats.frames_presented, 0u)
      << "a frame that was never displayed must not be counted as presented";
  EXPECT_GT(stats.frames_dropped, 0u)
      << "the frame reached the sink, so it has to be counted as a drop";
}

}  // namespace
}  // namespace avbase::media
