// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_MEDIA_FILTERS_NULL_VIDEO_SINK_H_
#define IJKPP_MEDIA_FILTERS_NULL_VIDEO_SINK_H_

#include <atomic>
#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "media/base/native_display.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"
#include "media/media_export.h"

namespace ijkpp::media {

// A video sink that renders nowhere but still drives the presentation cadence.
//
// WHY IT LIVES IN media/filters AND NOT platform/null: Chromium ships
// media/video/null_video_sink.h inside media/ for exactly this reason -- the
// headless path is part of the framework, not of a platform. Keeping it here
// is also what makes "zero configuration works" (docs/10 rule E1) hold on a
// machine with no window system at all: Player's default Deps leave the sink
// factories null, DefaultRendererFactory falls back to these, and playback
// runs end to end with nothing on screen. The platform backends (sdl2, linux)
// supply their own sinks through Deps when a real surface exists.
//
// CADENCE. The sink owns a dedicated thread that calls Render() at a fixed
// 60 Hz and drops whatever comes back, which is enough to pace the compositor
// (Δ18: the sink drives frame selection; the compositor decides). It reports
// no vsync interval (GetDisplayInterval() is false), so the compositor falls
// back to container-timestamp-driven cadence.
class IJKPP_MEDIA_EXPORT NullVideoSink final : public VideoRendererSink {
 public:
  NullVideoSink();
  NullVideoSink(const NullVideoSink&) = delete;
  NullVideoSink& operator=(const NullVideoSink&) = delete;
  ~NullVideoSink() override;

  // VideoRendererSink:
  void Initialize(RenderCallback* callback) override;
  void Start() override;
  void Stop() override;
  void Pause() override;
  void Play() override;
  void Flush() override;
  void SetOutputTarget(base::scoped_refptr<NativeDisplay> display) override;
  bool IsRunning() const override;
  bool GetDisplayInterval(base::TimeDelta* interval) const override;
  VideoSinkStats GetStats() const override;
  const char* name() const override { return "NullVideoSink"; }

 private:
  void RenderOne();

  std::unique_ptr<base::Thread> thread_;
  base::raw_ptr<RenderCallback> callback_{nullptr};
  base::scoped_refptr<base::SequencedTaskRunner> render_runner_;

  // Written from any thread, read on the render thread; single flags, so
  // atomics rather than a lock -- the render loop must never block on its
  // owner (the sink outlives every callback via VideoRendererImpl::Stop()).
  std::atomic<bool> playing_{false};
  std::atomic<bool> discard_{false};
  std::atomic<uint64_t> frames_presented_{0};
  std::atomic<uint64_t> frames_dropped_{0};
  std::atomic<uint64_t> submit_failures_{0};
};

class IJKPP_MEDIA_EXPORT NullVideoSinkFactory final
    : public VideoRendererSinkFactory {
 public:
  std::unique_ptr<VideoRendererSink> Create(
      base::scoped_refptr<NativeDisplay> display) override;
  const char* name() const override { return "null"; }
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_FILTERS_NULL_VIDEO_SINK_H_
