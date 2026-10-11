// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The --scene demo shared by the GL host examples.
//
// This is HOST POLICY, not library code, which is why it lives in examples/
// rather than in media/gl: the library hands a host a GlScene and a per-frame
// callback, and what goes into that scene is the application's decision. Two
// layouts are enough to exercise everything a scene does -- a second layer, a
// place on the canvas, a place in the z order, and a blend weight.
//
//   pip   the stream, delayed a fraction of a second, in an inset rect
//   fade  the same delayed copy filling the canvas, its opacity cycling
//
// Both need a SECOND image, and the player is single-source today. Rather than
// pull in a second decoder, the demo keeps a short ring of the frames the sink
// already handed it: a delayed copy of the same stream is a genuinely
// independent texture with its own upload, which is the property on trial.

#ifndef AVBASE_EXAMPLES_SCENE_DEMO_H_
#define AVBASE_EXAMPLES_SCENE_DEMO_H_

#include <cmath>
#include <cstddef>
#include <deque>
#include <numbers>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/native_display.h"
#include "media/base/render_perf_stats.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"

namespace avbase::examples {

class SceneDemo {
 public:
  // How far behind the second layer runs, in frames. ~0.2 s at 60 fps: enough
  // that the two layers are visibly different pictures, small enough that the
  // ring is not holding a large fraction of a second of 1080p.
  static constexpr size_t kDelayFrames = 12;
  // One full opacity cycle for the fade layout, in seconds. Long enough to
  // read as a dissolve rather than a flicker.
  static constexpr double kFadePeriodSeconds = 4.0;

  explicit SceneDemo(std::string mode) : mode_(std::move(mode)) {}
  SceneDemo(const SceneDemo&) = delete;
  SceneDemo& operator=(const SceneDemo&) = delete;
  ~SceneDemo() = default;

  static bool IsKnownMode(const std::string& mode) {
    return mode.empty() || mode == "pip" || mode == "fade";
  }
  bool enabled() const { return !mode_.empty(); }

  // The GlSceneDriver hook. Runs on the sink's render thread.
  void OnFrame(media::GlPresenter& presenter, media::GlScene& scene,
               media::GlScene::LayerId main_layer,
               const base::scoped_refptr<media::VideoFrame>& frame,
               media::PresentTiming* timing) {
    if (!enabled()) {
      return;
    }
    // The delayed frame is the one leaving the ring, not the one entering it,
    // so the offset is exactly kDelayFrames once the ring has filled.
    const base::scoped_refptr<media::VideoFrame> delayed =
        ring_.size() >= kDelayFrames ? ring_.front() : frame;
    ring_.push_back(frame);
    while (ring_.size() > kDelayFrames) {
      ring_.pop_front();
    }

    if (second_layer_ == media::GlScene::kNoLayer) {
      // z above the stream's layer (0), so the inset draws last.
      second_layer_ = scene.AddVideoLayer(10);
      if (second_layer_ == media::GlScene::kNoLayer) {
        return;
      }
      if (mode_ == "pip") {
        // Lower right, leaving a margin: the numbers are fractions of the
        // canvas, so the inset keeps its proportions when --canvas changes.
        scene.SetLayerRect(second_layer_, 0.62f, 0.54f, 0.34f, 0.42f);
      }
    }
    // Through the presenter, so this upload lands in the frame's upload_ms
    // alongside the main layer's: a second source that cost nothing would be
    // the one number the demo exists to show.
    presenter.UploadLayer(scene, second_layer_, *delayed, timing);

    if (mode_ == "fade") {
      if (start_.is_null()) {
        start_ = base::TimeTicks::Now();
      }
      const double seconds = (base::TimeTicks::Now() - start_).InSecondsF();
      // A smooth 0 -> 1 -> 0 rather than a sawtooth: the point is to watch the
      // blend itself, and a jump back to zero would look like a dropped frame.
      // std::numbers, not a literal and not M_PI: M_PI is a POSIX extension
      // and exists in this build only because CMAKE_CXX_EXTENSIONS defaults
      // to ON. media/filters/wsola_internals.h spells out its own kPi for the
      // same reason -- but that one is internal to a media target, and an
      // example reaching into media::internal would be worse than <numbers>.
      const double phase =
          2.0 * std::numbers::pi_v<double> * seconds / kFadePeriodSeconds;
      const float opacity = static_cast<float>(0.5 - 0.5 * std::cos(phase));
      scene.SetLayerOpacity(second_layer_, opacity);
    }
    (void)main_layer;
  }

 private:
  std::string mode_;
  std::deque<base::scoped_refptr<media::VideoFrame>> ring_;
  media::GlScene::LayerId second_layer_{media::GlScene::kNoLayer};
  base::TimeTicks start_;
};

// Wraps a host's sink factory so the demo's hook reaches the sink it creates.
//
// The sink is created INSIDE the player, so a factory is the only place a host
// can get at it between construction and the player taking it over -- and
// SetSceneUpdate()'s contract is exactly "before Start()".
//
// A template because the two hosts that needed it wrote it identically apart
// from the two types and the name string; a third should not add a third
// copy. |demo| doubles as the on/off switch: a null demo means no hook, which
// is the same thing the separate bool used to say with one more field to keep
// in agreement.
template <typename SinkFactory, typename Sink>
class SceneSinkFactory final : public media::VideoRendererSinkFactory {
 public:
  SceneSinkFactory(SceneDemo* demo, const char* name)
      : demo_(demo), name_(name) {}

  std::unique_ptr<media::VideoRendererSink>
  Create(base::scoped_refptr<media::NativeDisplay> display) override {
    std::unique_ptr<media::VideoRendererSink> sink =
        SinkFactory().Create(std::move(display));
    if (demo_ && demo_->enabled()) {
      // static_cast, not dynamic_cast: RTTI is off project-wide, and the
      // factory's contract is that it hands back a |Sink|.
      static_cast<Sink*>(sink.get())
          ->SetSceneUpdate(base::BindRepeating(&SceneDemo::OnFrame,
                                               base::Unretained(demo_)));
    }
    return sink;
  }

  const char* name() const override { return name_; }

 private:
  SceneDemo* demo_;  // Borrowed; owned by the example's main().
  const char* name_;
};

}  // namespace avbase::examples

#endif  // AVBASE_EXAMPLES_SCENE_DEMO_H_
