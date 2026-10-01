// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_NULL_AUDIO_SINK_H_
#define AVBASE_MEDIA_FILTERS_NULL_AUDIO_SINK_H_

#include <atomic>
#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_renderer_sink.h"
#include "media/media_export.h"

namespace avbase::media {

// An audio sink that consumes samples in real time and plays nothing.
//
// Same home as NullVideoSink (media/filters/, Chromium's media/audio/
// null_audio_sink precedent): the headless path is part of the framework.
// Consuming for real -- a thread that pulls Render() at device rate and
// discards -- matters more than for video: the audio clock is the default
// A/V master (Δ14), and a sink that faked consumption would stall it. Faking
// the consumption instead of skipping it also keeps underrun statistics
// meaningful in headless runs.
class AVBASE_MEDIA_EXPORT NullAudioSink final : public AudioRendererSink {
 public:
  NullAudioSink();
  NullAudioSink(const NullAudioSink&) = delete;
  NullAudioSink& operator=(const NullAudioSink&) = delete;
  ~NullAudioSink() override;

  // AudioRendererSink:
  void Initialize(const AudioParameters& params,
                  RenderCallback* callback) override;
  void Start() override;
  void Stop() override;
  void Pause() override;
  void Play() override;
  void Flush() override;
  bool SetVolume(double volume) override;
  bool IsOptimizedForHardwareParameters() override;
  bool CurrentThreadIsRenderingThread() override;
  base::TimeDelta GetHardwareLatency() const override;
  AudioGlitchInfo GetGlitchInfo() const override;
  const char* name() const override { return "NullAudioSink"; }

 private:
  // One device period of real-time consumption. Re-arms itself on the audio
  // thread's task runner; the period follows however much Render() actually
  // produced, so a starved clock does not fast-forward through silence.
  void ConsumeOnePeriod();

  std::unique_ptr<base::Thread> thread_;
  base::scoped_refptr<base::SequencedTaskRunner> audio_runner_;
  base::raw_ptr<RenderCallback> callback_{nullptr};
  AudioParameters params_;
  std::unique_ptr<AudioBus> bus_;

  std::atomic<bool> started_{false};
  std::atomic<bool> playing_{false};
  // Set on the audio thread at the top of ConsumeOnePeriod(); the sink's own
  // contract asks Render() implementations whether they run on the device
  // thread, and this is how the null backend answers truthfully.
  std::atomic<bool> on_rendering_thread_{false};
  // Written by the audio thread inside ConsumeOnePeriod(), read from any
  // thread through GetGlitchInfo().
  mutable base::Lock glitch_lock_;
  AudioGlitchInfo glitch_info_ GUARDED_BY(glitch_lock_);
};

class AVBASE_MEDIA_EXPORT NullAudioSinkFactory final
    : public AudioRendererSinkFactory {
 public:
  base::scoped_refptr<AudioRendererSink> Create() override;
  const char* name() const override { return "null"; }
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_NULL_AUDIO_SINK_H_
