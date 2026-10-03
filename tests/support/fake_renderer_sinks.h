// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The output side: sinks the test drives by hand. No device, no render thread.
//
// Both record what the renderer asked for, and both hand the test the
// RenderCallback the renderer registered, so a test can pull exactly as many
// frames or device periods as it wants. That is what makes "the ring drains, so
// EOS propagates" assertable: media/filters/null_audio_sink.cc does the same
// pulling for real, one device period at a time, on a timer.
//
// Everything the renderer's own sequences (S3/S4/S6) touch is atomic, because
// the test thread reads it concurrently -- a TSan run of the renderer suite
// reported the plain fields as data races. The frame vector is test-thread only
// and stays a plain container.

#ifndef AVBASE_TESTS_SUPPORT_FAKE_RENDERER_SINKS_H_
#define AVBASE_TESTS_SUPPORT_FAKE_RENDERER_SINKS_H_

#include <atomic>
#include <cstdint>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_renderer_sink.h"
#include "media/base/native_display.h"
#include "media/base/video_frame.h"
#include "media/base/video_renderer_sink.h"

namespace avbase::media::test {

class FakeVideoSink final : public VideoRendererSink {
 public:
  FakeVideoSink() = default;

  // Test control.
  RenderCallback* callback() const { return callback_.load(); }
  // Asks for up to |max_frames| display intervals' worth of frames, as a
  // running sink would, and keeps whatever came back. Returns how many frames
  // the renderer presented; nullptr answers (repeat the previous frame) are
  // counted but not stored.
  int PullFrames(int max_frames);
  const std::vector<base::scoped_refptr<VideoFrame>>& frames() const {
    return frames_;
  }
  void set_display_interval(base::TimeDelta interval) {
    display_interval_ = interval;
  }
  int start_count() const { return start_count_.load(); }
  int stop_count() const { return stop_count_.load(); }
  int pause_count() const { return pause_count_.load(); }
  int play_count() const { return play_count_.load(); }
  int flush_count() const { return flush_count_.load(); }

  // VideoRendererSink.
  void Initialize(RenderCallback* callback) override;
  void Start() override;
  void Stop() override;
  void Pause() override;
  void Play() override;
  void Flush() override;
  void SetOutputTarget(base::scoped_refptr<NativeDisplay> display) override;
  bool IsRunning() const override { return running_.load(); }
  bool GetDisplayInterval(base::TimeDelta* interval) const override;
  VideoSinkStats GetStats() const override;
  const char* name() const override { return "FakeVideoSink"; }

 private:
  std::atomic<RenderCallback*> callback_{nullptr};
  base::TimeDelta display_interval_ = base::Milliseconds(16);
  std::atomic<bool> running_{false};
  std::atomic<int> start_count_{0};
  std::atomic<int> stop_count_{0};
  std::atomic<int> pause_count_{0};
  std::atomic<int> play_count_{0};
  std::atomic<int> flush_count_{0};
  // Display intervals pulled so far and the frames they produced. Both are
  // touched by the test thread only.
  int pull_count_ = 0;
  std::vector<base::scoped_refptr<VideoFrame>> frames_;
};

class FakeAudioSink final : public AudioRendererSink {
 public:
  FakeAudioSink() = default;

  // Test control.
  RenderCallback* callback() const { return callback_.load(); }
  // Fills |dest| as one device period would. Returns the frames the renderer
  // wrote (0 means it had nothing and asked for silence).
  //
  // With |render_runner| set (pipeline-level tests), the callback is marshalled
  // onto that sequence and the call waits for it, instead of running inline on
  // the pulling thread. The real device thread is a sequence of its own; a
  // pipeline test's pulling thread is the gtest main thread, and an inline
  // render there lets renderer state be touched from two sequences at once
  // (TSan: SyntheticDemuxer::MakeAudioPacket via the pump the render callback
  // triggers). The RendererImpl-style suites leave it unset: single pump, no
  // second sequence to race with.
  int PullPeriod(AudioBus* dest);
  void
  set_render_runner(base::scoped_refptr<base::SequencedTaskRunner> runner) {
    render_runner_ = std::move(runner);
  }
  int start_count() const { return start_count_.load(); }
  int stop_count() const { return stop_count_.load(); }
  int pause_count() const { return pause_count_.load(); }
  int play_count() const { return play_count_.load(); }
  int flush_count() const { return flush_count_.load(); }
  double volume() const { return volume_.load(); }

  // AudioRendererSink.
  void Initialize(const AudioParameters& params,
                  RenderCallback* callback) override;
  void Start() override;
  void Stop() override;
  void Pause() override;
  void Play() override;
  void Flush() override;
  bool SetVolume(double volume) override;
  bool IsOptimizedForHardwareParameters() override { return true; }
  // The renderer DCHECKs this before touching the ring; a fake sink has no
  // separate render thread, so it is always "the right thread".
  bool CurrentThreadIsRenderingThread() override { return true; }
  base::TimeDelta GetHardwareLatency() const override {
    return base::TimeDelta();
  }
  AudioGlitchInfo GetGlitchInfo() const override { return AudioGlitchInfo(); }
  const char* name() const override { return "FakeAudioSink"; }

 private:
  ~FakeAudioSink() override = default;

  std::atomic<RenderCallback*> callback_{nullptr};
  base::scoped_refptr<base::SequencedTaskRunner> render_runner_;
  AudioParameters params_;
  std::atomic<bool> running_{false};
  std::atomic<int> start_count_{0};
  std::atomic<int> stop_count_{0};
  std::atomic<int> pause_count_{0};
  std::atomic<int> play_count_{0};
  std::atomic<int> flush_count_{0};
  std::atomic<double> volume_{1.0};
};

}  // namespace avbase::media::test

#endif  // AVBASE_TESTS_SUPPORT_FAKE_RENDERER_SINKS_H_
