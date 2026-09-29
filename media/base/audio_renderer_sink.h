// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Signature mirrors Chromium's `media/base/audio_renderer_sink.h`
// (BSD-3-Clause), including the Render(delay, delay_timestamp, glitch_info,
// AudioBus*) pull contract.

#ifndef IJKPP_MEDIA_BASE_AUDIO_RENDERER_SINK_H_
#define IJKPP_MEDIA_BASE_AUDIO_RENDERER_SINK_H_

#include <stdint.h>

#include <string>

#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/media_export.h"

namespace ijkpp::media {

// Underrun accounting. Exposed through PlaybackStats so that audio glitches
// stop being a "sometimes it crackles" bug report and become a metric.
struct IJKPP_MEDIA_EXPORT AudioGlitchInfo {
  uint64_t total_glitches{0};
  base::TimeDelta total_glitch_duration;
  uint64_t xruns{0};   // Device-level xruns (ALSA EPIPE, PulseAudio overrun).
};

class IJKPP_MEDIA_EXPORT AudioRendererSink
    : public base::RefCountedThreadSafe<AudioRendererSink> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  class RenderCallback {
   public:
    // Fills |dest| and returns the number of frames actually written; 0 means
    // "no data available, play silence".
    //
    // CONTRACT — this runs on the sink's audio thread and MUST NOT block,
    // allocate, take a lock held by any media sequence, or call back into
    // Player. Budget: < 100 us. Enforced by
    // tests/contract/audio_renderer_sink_contract.h.
    virtual int Render(base::TimeDelta delay,
                       base::TimeTicks delay_timestamp,
                       const AudioGlitchInfo& glitch_info,
                       AudioBus* dest) = 0;
    virtual void OnRenderError() = 0;

   protected:
    virtual ~RenderCallback() = default;
  };

  AudioRendererSink(const AudioRendererSink&) = delete;
  AudioRendererSink& operator=(const AudioRendererSink&) = delete;

  virtual void Initialize(const AudioParameters& params,
                          RenderCallback* callback) = 0;
  virtual void Start() = 0;
  // After Stop() returns, Render() is guaranteed never to be called again.
  virtual void Stop() = 0;
  virtual void Pause() = 0;
  virtual void Play() = 0;
  virtual void Flush() = 0;   // Only valid while not playing.
  virtual bool SetVolume(double volume) = 0;   // [0.0, 1.0]
  virtual bool IsOptimizedForHardwareParameters() = 0;
  virtual bool CurrentThreadIsRenderingThread() = 0;
  // Hardware latency, used to correct the audio clock. Returns zero when the
  // backend cannot report it, which costs A/V sync accuracy.
  virtual base::TimeDelta GetHardwareLatency() const = 0;
  virtual AudioGlitchInfo GetGlitchInfo() const = 0;
  virtual const char* name() const = 0;

 protected:
  friend class base::RefCountedThreadSafe<AudioRendererSink>;
  AudioRendererSink() = default;
  virtual ~AudioRendererSink() = default;
};

// A sink that can be Initialize()d and Start()ed again after Stop().
class IJKPP_MEDIA_EXPORT RestartableAudioRendererSink : public AudioRendererSink {
 protected:
  ~RestartableAudioRendererSink() override = default;
};

class IJKPP_MEDIA_EXPORT AudioRendererSinkFactory {
 public:
  AudioRendererSinkFactory(const AudioRendererSinkFactory&) = delete;
  AudioRendererSinkFactory& operator=(const AudioRendererSinkFactory&) = delete;

  virtual base::scoped_refptr<AudioRendererSink> Create() = 0;
  virtual const char* name() const = 0;

 protected:
  AudioRendererSinkFactory() = default;
  virtual ~AudioRendererSinkFactory() = default;
};

}  // namespace ijkpp::media

#endif  // IJKPP_MEDIA_BASE_AUDIO_RENDERER_SINK_H_
