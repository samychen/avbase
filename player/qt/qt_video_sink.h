// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The Qt video output endpoint.
//
// WHY THIS SHAPE. avbase already ships three window-system sinks (SDL2, GLFW,
// EGL) and all three are thin adapters over GlVideoSink: the backend only
// supplies a NativeGLContext and the shared sink does the drawing. Qt is not
// one of those, because Qt insists on owning the paint of its own window, and
// a GL context bound to a QQuickWindow cannot legally be made current from
// avbase's render thread. Rather than add a fourth window-system backend, this
// app takes the seam avbase already exposes for exactly this case: a custom
// VideoRendererSink. It renders nowhere, polls the compositor at display rate,
// and hands each frame to the GUI thread through a FrameMailbox.
//
// The cost is one YUV->RGB conversion and one texture upload per painted
// frame, on the CPU. That is fine for a UI skeleton and for 1080p30; a
// production player should grow a platform/qt/ backend with a NativeGLContext
// (5 methods -- see render/gl/native_gl_context.h) and let avbase draw straight
// into Qt's FBO. The public shape of this file would not change.

#ifndef AVBASE_PLAYER_QT_QT_VIDEO_SINK_H_
#define AVBASE_PLAYER_QT_QT_VIDEO_SINK_H_

#include <atomic>
#include <memory>
#include <mutex>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "media/base/native_display.h"
#include "media/base/video_renderer_sink.h"
#include "player/qt/frame_mailbox.h"

namespace avbase::qt {

// A sink that renders nowhere and keeps the newest frame in |mailbox|.
//
// The pull loop runs on a base::Thread, not a std::thread: this file is meant
// to be copied by a host, and base::Thread is what gives it a task queue and a
// bounded Stop() that joins. The 60 Hz cadence is one chained
// PostDelayedTask -- the same shape NullVideoSink and examples/pull_frames.cc
// use, so stopping is "the next arm never happens, plus Thread::Stop()".
class QtVideoSink final : public media::VideoRendererSink {
 public:
  explicit QtVideoSink(FrameMailbox* mailbox);
  QtVideoSink(const QtVideoSink&) = delete;
  QtVideoSink& operator=(const QtVideoSink&) = delete;
  ~QtVideoSink() override;

  // media::VideoRendererSink:
  void Initialize(RenderCallback* callback) override;
  void Start() override;
  void Stop() override;
  void Pause() override;
  void Play() override;
  void Flush() override;
  void
  SetOutputTarget(base::scoped_refptr<media::NativeDisplay> display) override;
  bool IsRunning() const override;
  bool GetDisplayInterval(base::TimeDelta* interval) const override;
  media::VideoSinkStats GetStats() const override;
  const char* name() const override { return "qt-mailbox"; }

 private:
  // Everything the poll loop touches lives here rather than in the sink, and
  // the loop holds a shared_ptr to it instead of a `this`. That is what makes
  // the shutdown race disappear: Stop() can flip running=false, join, and let
  // the sink be destroyed while a queued poll is still alive, because that poll
  // has no way back to the sink. Binding `this` instead crashed within a second
  // of playback (SIGSEGV in PostDelayedTask, then an invalid std::mutex).
  struct Context;
  static void PollOnce(std::shared_ptr<Context> context);

  FrameMailbox* mailbox_;
  // Guards worker_/context_ between Start(), Stop() and ~QtVideoSink.
  mutable std::mutex mutex_;
  std::unique_ptr<base::Thread> worker_;
  std::shared_ptr<Context> context_;
  RenderCallback* upstream_{nullptr};
};

class QtVideoSinkFactory final : public media::VideoRendererSinkFactory {
 public:
  explicit QtVideoSinkFactory(FrameMailbox* mailbox) : mailbox_(mailbox) {}
  std::unique_ptr<media::VideoRendererSink>
  Create(base::scoped_refptr<media::NativeDisplay> display) override;
  const char* name() const override { return "qt-mailbox-factory"; }

 private:
  FrameMailbox* mailbox_;
};

}  // namespace avbase::qt

#endif  // AVBASE_PLAYER_QT_QT_VIDEO_SINK_H_
