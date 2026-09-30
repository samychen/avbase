// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/video_renderer_sink.h` (BSD-3-Clause).

#ifndef IJKPP_MEDIA_BASE_VIDEO_RENDERER_SINK_H_
#define IJKPP_MEDIA_BASE_VIDEO_RENDERER_SINK_H_

#include <stdint.h>

#include <memory>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/native_display.h"
#include "media/base/video_frame.h"
#include "media/media_export.h"

namespace ijkpp::media {

struct IJKPP_MEDIA_EXPORT VideoSinkStats {
  uint64_t frames_presented{0};
  uint64_t frames_dropped{0};
  uint64_t submit_failures{0};
  double measured_refresh_hz{0.0};
};

// The video output endpoint.
//
// The sink owns the display cadence: it calls Render() on its own render
// sequence and asks the compositor for whatever frame belongs in the current
// refresh window. This is Chromium's model and it is what makes vsync-accurate
// presentation possible on X11 (Present extension) and Wayland
// (wp_presentation_feedback). See docs/04 §1.1 and behaviour difference Δ18.
class IJKPP_MEDIA_EXPORT VideoRendererSink {
 public:
  class RenderCallback {
   public:
    // Returns the frame to display, or nullptr to keep showing the previous
    // one. |deadline_min|/|deadline_max| bracket the display window; a null
    // |deadline_max| means the sink has no vsync information.
    virtual base::scoped_refptr<VideoFrame> Render(
        base::TimeTicks deadline_min, base::TimeTicks deadline_max) = 0;
    virtual void OnFrameSubmitFailure() = 0;

   protected:
    virtual ~RenderCallback() = default;
  };

  VideoRendererSink(const VideoRendererSink&) = delete;
  VideoRendererSink& operator=(const VideoRendererSink&) = delete;

  virtual void Initialize(RenderCallback* callback) = 0;
  virtual void Start() = 0;
  virtual void Stop() = 0;
  virtual void Pause() = 0;
  virtual void Play() = 0;
  virtual void Flush() = 0;   // Only valid while not playing.
  // May be called while playing, from any thread; implementations must
  // serialise it against Render().
  virtual void SetOutputTarget(base::scoped_refptr<NativeDisplay> display) = 0;
  virtual bool IsRunning() const = 0;
  // Fills |interval| with the display refresh period. Returns false when the
  // sink cannot determine it, in which case the compositor falls back to a
  // timer-driven cadence.
  virtual bool GetDisplayInterval(base::TimeDelta* interval) const = 0;
  virtual VideoSinkStats GetStats() const = 0;
  virtual const char* name() const = 0;

  // PUBLIC, and that is load-bearing. VideoRendererSink is not ref-counted --
  // VideoRendererSinkFactory::Create() hands it back in a std::unique_ptr, and
  // default_delete<VideoRendererSink> calls `delete` through a
  // VideoRendererSink*, which needs an accessible destructor. A protected one
  // makes every unique_ptr<VideoRendererSink> ill-formed at the point of
  // destruction, so Create() could never have been called. This is not a new
  // constraint introduced by RendererImpl; it is a latent defect in this header
  // that nothing instantiated until the tenth round.
  //
  // Contrast AudioRendererSink, whose destructor is correctly protected: it IS
  // ref-counted, befriends base::RefCountedThreadSafe<AudioRendererSink>, and
  // Release() performs the delete from inside that friend. The two sinks look
  // alike and are owned differently, which is why one of them was
  // wrong.
  virtual ~VideoRendererSink() = default;

 protected:
  VideoRendererSink() = default;
};

class IJKPP_MEDIA_EXPORT VideoRendererSinkFactory {
 public:
  VideoRendererSinkFactory(const VideoRendererSinkFactory&) = delete;
  VideoRendererSinkFactory& operator=(const VideoRendererSinkFactory&) = delete;

  virtual std::unique_ptr<VideoRendererSink> Create(
      base::scoped_refptr<NativeDisplay> display) = 0;
  virtual const char* name() const = 0;

 protected:
  VideoRendererSinkFactory() = default;
  virtual ~VideoRendererSinkFactory() = default;
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_VIDEO_RENDERER_SINK_H_
