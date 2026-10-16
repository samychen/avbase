// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The --scene demo shared by the GL host examples.
//
// This is HOST POLICY, not library code, which is why it lives in examples/
// rather than in the library: the SDK hands a host a Scene and a per-frame
// callback, and what goes into that scene is the application's decision. Two
// layouts are enough to exercise everything a scene does -- a second layer, a
// place on the canvas, a place in the z order, and a blend weight.
//
//   pip   the stream, delayed a fraction of a second, in an inset rect
//   fade  the same delayed copy filling the canvas, its opacity cycling
//
// Both need a SECOND image, and for most of this file's life a second image
// meant a second PLAYER -- its own demuxer, decoder and clock, publishing into
// a SceneSource through that source's publisher sink factory. That shape is
// gone as of docs/14 stage 3: the second image now comes from a second SOURCE
// registered on the same player, and the two are decoded by one pipeline
// against one clock. What the demo does on screen did not change.
//
//   * by default, a short ring of the frames the sink already handed it -- a
//     delayed copy of the same stream is a genuinely independent texture with
//     its own upload, which is the property on trial;
//   * with --second <url>, a second demuxer in the same pipeline, whose sink
//     publishes into the SceneSource the registration returned.
//
// The difference is the kind of thing that used to be invisible: two pipelines
// never agree on a clock, and the mailbox was the truce between them. One
// pipeline has nothing to reconcile -- its renderer asks every source for the
// frame that belongs in this refresh window, and the display's clock stays the
// only one that decides what is shown.
//
// EVERYTHING HERE GOES THROUGH Player::scene(). It used to need a bespoke
// VideoRendererSinkFactory that static_cast the sink it was handed down to
// GlVideoSink -- which worked only because the author of that cast also wrote
// the factory it was aiming at (RTTI is off, so nothing checked it), and which
// is undefined behaviour the moment the default nodeps player gets its
// NullVideoSink instead. See docs/TODO.md §2.

#ifndef AVBASE_EXAMPLES_SCENE_DEMO_H_
#define AVBASE_EXAMPLES_SCENE_DEMO_H_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <numbers>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/video_frame.h"
// No Deps and no NullAudioSinkFactory any more: those belonged to the second
// Player this file used to build. A second source needs neither -- no second
// output to route, and no second master clock to muzzle.
#include "player/public/player.h"
#include "player/public/scene.h"

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
  // Above the player's own layer (z = 0), so the inset draws last.
  static constexpr int kSecondZ = 10;

  explicit SceneDemo(std::string mode) : mode_(std::move(mode)) {}
  SceneDemo(const SceneDemo&) = delete;
  SceneDemo& operator=(const SceneDemo&) = delete;
  ~SceneDemo() = default;

  static bool IsKnownMode(const std::string& mode) {
    return mode.empty() || mode == "pip" || mode == "fade";
  }
  bool enabled() const { return !mode_.empty(); }

  // The whole of what wiring this demo up used to be.
  //
  // Note WHEN this can be called: after construction but BEFORE SetDataSource
  // and PrepareAsync. The scene is not part of a pipeline, but a registered
  // source IS: the player turns it into a demuxer at PrepareAsync, so a
  // registration made later is refused (and says so by returning null) rather
  // than quietly never being fed.
  //
  // Everything past registration is unchanged by that: the callback and the
  // source are plumbed into whichever renderer gets built later, and into the
  // next one if the player is reset.
  void Install(avbase::Player& player) {
    if (!enabled()) {
      return;
    }
    player.scene()->SetUpdateCallback(
        base::BindRepeating(&SceneDemo::OnFrame, base::Unretained(this)));
    if (!second_url_.empty()) {
      // One registration instead of a whole second Player instance. Source 0
      // is whichever URL SetDataSource() gets; this is source 1, and its
      // decoded video lands in the layer this returns.
      second_ =
          player.AddSource(avbase::DataSourceDescriptor::FromUri(second_url_),
                           "second", kSecondZ);
      if (!second_) {
        std::fprintf(stderr,
                     "--second: AddSource failed (state past "
                     "kInitialized?); falling back to the delayed copy\n");
      }
    }
  }

  void set_second_url(std::string url) { second_url_ = std::move(url); }
  std::shared_ptr<avbase::SceneSource> second_source() const { return second_; }
  // How many frames the hook actually placed a second layer on. The number
  // that tells a host "the scene ran" from "the scene was configured and
  // nothing happened": 0 with a bound scene means the hook never fired.
  uint64_t frames_placed() const { return frames_placed_; }

  // The per-frame hook. Runs on the sink's render thread.
  //
  // Note what it does NOT do: upload, in the --second case. A registered source
  // is synced by the scene, which is the whole point of registering one -- the
  // hook's job is placement, and the only upload here is the fallback path that
  // feeds the layer from the ring when no second source was wired up.
  void OnFrame(avbase::SceneFrame& frame,
               const base::scoped_refptr<media::VideoFrame>& main_frame) {
    if (!enabled()) {
      return;
    }
    avbase::SceneLayer second;
    if (second_ != nullptr) {
      second = frame.layer_of(*second_);
      if (!second) {
        // The scene has not synced this source yet, so there is no layer to
        // place. It will have one from the next frame on. This is what the
        // old code needed has_layer() for, and it is still a real case: the
        // layer is made on the GL thread, so it cannot exist before the first
        // frame that draws it.
        return;
      }
    } else {
      // No second decoder: the delayed frame is the one leaving the ring, not
      // the one entering it, so the offset is exactly kDelayFrames once the
      // ring has filled.
      base::scoped_refptr<media::VideoFrame> delayed =
          ring_.size() >= kDelayFrames ? ring_.front() : main_frame;
      ring_.push_back(main_frame);
      while (ring_.size() > kDelayFrames) {
        ring_.pop_front();
      }
      if (second_layer_ == 0) {
        second_layer_ = frame.AddLayer(kSecondZ).id();
        if (second_layer_ == 0) {
          return;
        }
      }
      second = frame.layer_with_id(second_layer_);
      second.Upload(*delayed);
    }

    // Placement is host policy either way, and it is the only thing a
    // registered source cannot decide for itself.
    if (mode_ == "pip") {
      // Lower right, leaving a margin: the numbers are fractions of the
      // canvas, so the inset keeps its proportions when --canvas changes.
      second.SetRect(0.62f, 0.54f, 0.34f, 0.42f);
      // Fit rather than stretch: the inset is not the canvas's shape, and
      // the demo exists partly to show that the choice is visible.
      second.SetContentMode(avbase::SceneContentMode::kFit);
    } else {
      second.SetRect(0.0f, 0.0f, 1.0f, 1.0f);
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
      second.SetOpacity(static_cast<float>(0.5 - 0.5 * std::cos(phase)));
    }
    ++frames_placed_;
  }

 private:
  std::string mode_;
  std::string second_url_;
  std::deque<base::scoped_refptr<media::VideoFrame>> ring_;
  // A layer id rather than a SceneLayer: the frame object differs every frame,
  // so only the id carries across.
  uint64_t second_layer_{0};
  std::shared_ptr<avbase::SceneSource> second_;
  uint64_t frames_placed_{0};
  base::TimeTicks start_;  // Set on the first frame; the fade's phase zero.
};

// Reports what the second source's mailbox saw.
//
// The teardown ORDERING that used to be documented here -- "stop the producer
// before the consumer" -- is gone with the second pipeline. There is one
// pipeline now, so there is nothing whose producer has to be quietened first:
// stopping the player stops every source at once. That simplification is most
// of what stage 3 was for.
inline void ReportSecondSource(avbase::SceneSource& source) {
  // published - consumed - superseded == still waiting (0 or 1). Printed once,
  // at the end, because that is the only number that says whether the second
  // decoder really fed the second layer: a source that produced N frames and
  // had all N superseded was being fed faster than the layer was drawn, which
  // is fine, and one that produced 0 was never wired up at all.
  //
  // These counters come from the SceneSource rather than from the renderer, so
  // they still add up after the pipeline has been stopped.
  std::fprintf(stderr,
               "--second: published=%llu consumed=%llu superseded=%llu\n",
               static_cast<unsigned long long>(source.frames_published()),
               static_cast<unsigned long long>(source.frames_consumed()),
               static_cast<unsigned long long>(source.frames_superseded()));
}

}  // namespace avbase::examples

#endif  // AVBASE_EXAMPLES_SCENE_DEMO_H_
