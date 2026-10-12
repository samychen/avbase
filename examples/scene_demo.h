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
// Both need a SECOND image, and the player is single-source today. The demo
// takes that image from one of two places:
//
//   * by default, a short ring of the frames the sink already handed it -- a
//     delayed copy of the same stream is a genuinely independent texture with
//     its own upload, which is the property on trial;
//   * with --second <url>, a SECOND Player: its own demuxer, its own decoder,
//     its own clock, publishing into a GlLayerSource through
//     examples::PublisherSink.
//
// The second one is the real thing, and it is the reason GlLayerSource is a
// mailbox rather than a layer: two pipelines never agree on a clock, and the
// only clock here that decides what is shown is the display's.

#ifndef AVBASE_EXAMPLES_SCENE_DEMO_H_
#define AVBASE_EXAMPLES_SCENE_DEMO_H_

#include <cmath>
#include <cstddef>
#include <deque>
#include <memory>
#include <numbers>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "examples/publisher_sink.h"
#include "media/base/native_display.h"
#include "media/base/render_perf_stats.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/filters/null_audio_sink.h"
#include "media/filters/null_video_sink.h"
#include "media/gl/gl_layer_source.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"
#include "media/gl/gl_video_sink.h"
#include "player/public/deps.h"
#include "player/public/player.h"

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

  // The mailbox the second layer's frames arrive in, when the host wired a
  // second Player into it (--second). Null means the delayed-copy fallback
  // below is what feeds the layer.
  void set_second_source(std::shared_ptr<media::GlLayerSource> source) {
    second_source_ = std::move(source);
  }

  // The GlSceneEngine hook. Runs on the sink's render thread.
  //
  // Note what it does NOT do: upload. A registered source is synced by the
  // engine, which is the whole point of registering one -- the hook's job is
  // placement, and the only upload here is the fallback path that feeds the
  // layer from the ring when no second source was wired up.
  void OnFrame(media::GlPresenter& presenter, media::GlScene& scene,
               media::GlScene::LayerId main_layer,
               const base::scoped_refptr<media::VideoFrame>& frame,
               media::PresentTiming* timing) {
    if (!enabled()) {
      return;
    }
    media::GlScene::LayerId second = media::GlScene::kNoLayer;
    if (second_source_ != nullptr) {
      second = second_source_->has_layer() ? second_source_->layer()
                                           : media::GlScene::kNoLayer;
      if (second == media::GlScene::kNoLayer) {
        // The engine has not synced this source yet, so there is no layer to
        // place. It will have one from the next frame on.
        return;
      }
    } else {
      // No second decoder: the delayed frame is the one leaving the ring, not
      // the one entering it, so the offset is exactly kDelayFrames once the
      // ring has filled.
      base::scoped_refptr<media::VideoFrame> delayed =
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
      }
      second = second_layer_;
      // Through the presenter, so this upload lands in the frame's upload_ms
      // alongside the main layer's: a second source that cost nothing would be
      // the one number the demo exists to show.
      presenter.UploadLayer(scene, second, *delayed, timing);
    }

    // Placement is host policy either way, and it is the only thing a
    // registered source cannot decide for itself.
    if (mode_ == "pip") {
      // Lower right, leaving a margin: the numbers are fractions of the
      // canvas, so the inset keeps its proportions when --canvas changes.
      scene.SetLayerRect(second, 0.62f, 0.54f, 0.34f, 0.42f);
    } else {
      scene.SetLayerRect(second, 0.0f, 0.0f, 1.0f, 1.0f);
      if (start_.is_null()) {
        start_ = base::TimeTicks::Now();
      }
      const double seconds = (base::TimeTicks::Now() - start_).InSecondsF();
      // A smooth 0 -> 1 -> 0 rather than a sawtooth: the point is to watch
      // the blend itself, and a jump back to zero would look like a dropped
      // frame.
      //
      // std::numbers, not a literal and not M_PI: M_PI is a POSIX extension
      // and exists in this build only because CMAKE_CXX_EXTENSIONS defaults
      // to ON. media/filters/wsola_internals.h spells out its own kPi for the
      // same reason -- but that one is internal to a media target, and an
      // example reaching into media::internal would be worse than <numbers>.
      const double phase =
          2.0 * std::numbers::pi_v<double> * seconds / kFadePeriodSeconds;
      scene.SetLayerOpacity(second,
                            static_cast<float>(0.5 - 0.5 * std::cos(phase)));
    }
    (void)main_layer;
  }

 private:
  std::string mode_;
  std::deque<base::scoped_refptr<media::VideoFrame>> ring_;
  media::GlScene::LayerId second_layer_{media::GlScene::kNoLayer};
  std::shared_ptr<media::GlLayerSource> second_source_;
  base::TimeTicks start_;  // Set on the first frame; the fade's phase zero.
};

// Builds the SECOND pipeline: its own Player, decoding |url| independently of
// the first, presenting into |source| through a PublisherSink instead of a
// window. Audio goes to the null sink -- one master clock per process, and the
// first player's is it.
//
// Returns null when the URL cannot be opened. The caller owns the result and
// must StopSync() it before the first player's sink goes away, because the
// source it publishes into belongs to that sink's engine.
inline std::unique_ptr<avbase::Player>
StartSecondSource(std::string url,
                  std::shared_ptr<media::GlLayerSource> source) {
  DCHECK(source);
  auto deps = std::make_unique<avbase::Deps>();
  deps->video_sink_factory =
      std::make_shared<PublisherSinkFactory>(std::move(source));
  deps->audio_sink_factory =
      std::make_shared<avbase::media::NullAudioSinkFactory>();

  auto player =
      std::make_unique<avbase::Player>(avbase::PlayerConfig(), std::move(deps));
  if (avbase::Status s = player->SetDataSource(url); !s) {
    std::fprintf(stderr, "--second: SetDataSource failed: %s\n",
                 s.error().ToString().c_str());
    return nullptr;
  }
  // PrepareSync() also STARTS it (PlayerConfig::start_on_prepared), which is
  // exactly what a second source wants: decode from here on, at its own pace,
  // with no window and no clock of its own. Calling Start() on top of it is the
  // "Start in state Started is not legal" error.
  if (avbase::Status s = player->PrepareSync(); !s) {
    std::fprintf(stderr, "--second: Prepare failed: %s\n",
                 s.error().ToString().c_str());
    return nullptr;
  }
  return player;
}

// Tears the second pipeline down and reports what the mailbox saw.
//
// MUST run before the first player's sink is destroyed: the source belongs to
// that sink's engine, and the producer has to be quiet first -- the same
// "stop the producer, then the consumer" order the sinks rely on for the
// presenter.
inline void StopSecondSource(std::unique_ptr<avbase::Player> player,
                             std::shared_ptr<media::GlLayerSource> source) {
  if (player) {
    player->StopSync(avbase::base::Seconds(5));
  }
  if (!source) {
    return;
  }
  // published - consumed - superseded == still waiting (0 or 1). Printed once,
  // at the end, because that is the only number that says whether the second
  // decoder really fed the second layer: a source that produced N frames and
  // had all N superseded was being polled faster than it could decode, which
  // is fine, and one that produced 0 was never wired up at all.
  std::fprintf(stderr,
               "--second: published=%llu consumed=%llu superseded=%llu\n",
               static_cast<unsigned long long>(source->frames_published()),
               static_cast<unsigned long long>(source->frames_consumed()),
               static_cast<unsigned long long>(source->frames_superseded()));
  player.reset();
  source->Detach(nullptr);
}

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
  // |demo| is the layout hook; null means none. |second_source|, when non-null,
  // receives a source freshly registered with the sink's engine -- the handle a
  // second Player's PublisherSink then publishes into.
  SceneSinkFactory(
      SceneDemo* demo, const char* name,
      std::shared_ptr<media::GlLayerSource>* second_source = nullptr,
      int second_z = 10)
      : demo_(demo),
        name_(name),
        second_source_(second_source),
        second_z_(second_z) {}

  std::unique_ptr<media::VideoRendererSink>
  Create(base::scoped_refptr<media::NativeDisplay> display) override {
    std::unique_ptr<media::VideoRendererSink> sink =
        SinkFactory().Create(std::move(display));
    if (demo_ == nullptr || !demo_->enabled()) {
      return sink;
    }
    // static_cast, not dynamic_cast: RTTI is off project-wide, and the
    // factory's contract is that it hands back a |Sink|.
    Sink& typed = static_cast<Sink&>(*sink);
    if (second_source_ != nullptr) {
      // The source has to be registered with the engine the SINK owns, so this
      // is the only moment it can be created: the player takes the sink over
      // as soon as Create() returns.
      *second_source_ = typed.AddSceneSource("second", second_z_);
    }
    typed.SetSceneUpdate(
        base::BindRepeating(&SceneDemo::OnFrame, base::Unretained(demo_)));
    return sink;
  }

  const char* name() const override { return name_; }

 private:
  SceneDemo* demo_;  // Borrowed; owned by the example's main().
  const char* name_;
  std::shared_ptr<media::GlLayerSource>* second_source_;
  int second_z_;
};

}  // namespace avbase::examples

#endif  // AVBASE_EXAMPLES_SCENE_DEMO_H_
