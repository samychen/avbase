// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLATFORM_SDL2_SDL2_AUDIO_SINK_H_
#define AVBASE_PLATFORM_SDL2_SDL2_AUDIO_SINK_H_

#include <atomic>
#include <cstdint>
#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_renderer_sink.h"
#include "media/media_export.h"

namespace avbase::media {

// The SDL2 audio output endpoint: SDL_OpenAudioDevice in callback mode. SDL's
// audio thread is S7 -- it pulls Render() once per device period, converts the
// planar AudioBus to interleaved float for SDL, and nothing else (Δ13: no DSP
// here, the WSOLA work happened on S4).
class AVBASE_MEDIA_EXPORT Sdl2AudioSink final : public AudioRendererSink {
 public:
  Sdl2AudioSink();
  Sdl2AudioSink(const Sdl2AudioSink&) = delete;
  Sdl2AudioSink& operator=(const Sdl2AudioSink&) = delete;
  ~Sdl2AudioSink() override;

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
  const char* name() const override { return "Sdl2AudioSink"; }

 private:
  static void AudioCallbackTrampoline(void* userdata, uint8_t* stream,
                                      int len);
  void AudioCallback(uint8_t* stream, int len);

  base::raw_ptr<RenderCallback> callback_{nullptr};
  AudioParameters params_;
  std::unique_ptr<AudioBus> bus_;
  // Owned here, used only from SDL's audio thread after Start().
  std::unique_ptr<float[]> interleaved_;

  void* device_{nullptr};     // SDL_AudioDeviceID, kept as void* like the
                              // video sink keeps its renderer.
  std::atomic<bool> initialized_{false};
  std::atomic<bool> started_{false};
  std::atomic<bool> playing_{false};
  std::atomic<bool> on_rendering_thread_{false};
  std::atomic<uint64_t> underruns_{0};
  // Written by SDL's audio thread inside the callback, read from any thread
  // through GetGlitchInfo().
  mutable base::Lock glitch_lock_;
  AudioGlitchInfo glitch_info_ GUARDED_BY(glitch_lock_);
};

class AVBASE_MEDIA_EXPORT Sdl2AudioSinkFactory final
    : public AudioRendererSinkFactory {
 public:
  base::scoped_refptr<AudioRendererSink> Create() override;
  const char* name() const override { return "sdl2"; }
};

}  // namespace avbase::media

#endif  // AVBASE_PLATFORM_SDL2_SDL2_AUDIO_SINK_H_
