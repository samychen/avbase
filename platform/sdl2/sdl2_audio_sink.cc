// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/sdl2/sdl2_audio_sink.h"

#include <algorithm>
#include <cstring>

#include "SDL.h"

#include "base/logging.h"

namespace avbase::media {

Sdl2AudioSink::Sdl2AudioSink() = default;

Sdl2AudioSink::~Sdl2AudioSink() {
  // After Stop() returns SDL's audio thread is gone, so no callback can
  // touch the bus or the callback pointer while they are destroyed.
  Stop();
}

void Sdl2AudioSink::Initialize(const AudioParameters& params,
                               RenderCallback* callback) {
  params_ = params;
  callback_ = callback;
  bus_ = AudioBus::Create(params.channels(), params.frames_per_buffer());
  bus_->Zero();
  interleaved_ = std::make_unique<float[]>(
      static_cast<size_t>(params.channels()) *
      static_cast<size_t>(params.frames_per_buffer()));
  initialized_.store(true);
}

void Sdl2AudioSink::Start() {
  if (started_.exchange(true)) {
    return;
  }
  // The host may have initialised only video; audio is initialised lazily
  // here so that a video-only host is not forced to name audio at startup.
  if ((SDL_WasInit(0) & SDL_INIT_AUDIO) == 0) {
    SDL_InitSubSystem(SDL_INIT_AUDIO);
  }
  SDL_AudioSpec want{};
  want.freq = params_.sample_rate();
  want.format = AUDIO_F32SYS;
  want.channels = static_cast<Uint8>(params_.channels());
  want.samples = static_cast<Uint16>(params_.frames_per_buffer());
  want.callback = &Sdl2AudioSink::AudioCallbackTrampoline;
  want.userdata = this;
  SDL_AudioSpec have{};
  // allowed_changes = 0: SDL converts internally if the device differs, so
  // the callback always fills the requested format (the AudioBus matches
  // |params_|, and SDL's conversion runs before our buffer reaches the wire).
  SDL_AudioDeviceID device =
      SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (device == 0) {
    LOG(ERROR) << "avbase.aout: SDL_OpenAudioDevice failed: "
               << SDL_GetError();
    started_.store(false);
    return;
  }
  device_ = reinterpret_cast<void*>(static_cast<uintptr_t>(device));
}

void Sdl2AudioSink::Stop() {
  if (device_ != nullptr) {
    const SDL_AudioDeviceID device =
        static_cast<SDL_AudioDeviceID>(
            reinterpret_cast<uintptr_t>(device_));
    // Close pauses and destroys the device: after this returns, the callback
    // is never invoked again (the AudioRendererSink contract).
    SDL_CloseAudioDevice(device);
    device_ = nullptr;
  }
  started_.store(false);
  playing_.store(false);
}

void Sdl2AudioSink::Pause() {
  if (device_ != nullptr) {
    SDL_PauseAudioDevice(
        static_cast<SDL_AudioDeviceID>(
            reinterpret_cast<uintptr_t>(device_)), 1);
  }
  playing_.store(false);
}

void Sdl2AudioSink::Play() {
  if (device_ != nullptr) {
    SDL_PauseAudioDevice(
        static_cast<SDL_AudioDeviceID>(
            reinterpret_cast<uintptr_t>(device_)), 0);
    playing_.store(true);
  }
}

void Sdl2AudioSink::Flush() {
  if (device_ != nullptr) {
    // Only valid while not playing; AudioRendererImpl::Flush() guarantees
    // that ordering (it pauses first, for exactly this reason).
    SDL_ClearQueuedAudio(
        static_cast<SDL_AudioDeviceID>(
            reinterpret_cast<uintptr_t>(device_)));
  }
}

bool Sdl2AudioSink::SetVolume(double volume) {
  // AudioRendererImpl scales the samples before they get here (the volume
  // atomic in Render's caller chain); applying it again here would square it.
  (void)volume;
  return true;
}

bool Sdl2AudioSink::IsOptimizedForHardwareParameters() {
  return false;
}

bool Sdl2AudioSink::CurrentThreadIsRenderingThread() {
  return on_rendering_thread_.load();
}

base::TimeDelta Sdl2AudioSink::GetHardwareLatency() const {
  // SDL does not expose device latency; the queued-samples estimate is what
  // the audio clock correction (docs/04 §6) has to work with. Zero would be
  // wrong by half a period, which is audible; one period is close enough.
  if (params_.sample_rate() <= 0) {
    return base::TimeDelta();
  }
  return base::SecondsD(static_cast<double>(params_.frames_per_buffer()) /
                        params_.sample_rate());
}

AudioGlitchInfo Sdl2AudioSink::GetGlitchInfo() const {
  base::AutoLock scoped(glitch_lock_);
  return glitch_info_;
}

void Sdl2AudioSink::AudioCallbackTrampoline(void* userdata, uint8_t* stream,
                                            int len) {
  static_cast<Sdl2AudioSink*>(userdata)->AudioCallback(stream, len);
}

void Sdl2AudioSink::AudioCallback(uint8_t* stream, int len) {
  on_rendering_thread_.store(true);
  if (!initialized_.load() || !callback_ || !bus_) {
    std::memset(stream, 0, static_cast<size_t>(len));
    return;
  }
  const int channels = params_.channels();
  const base::TimeTicks now = base::TimeTicks::Now();
  const AudioGlitchInfo none;
  const int written = callback_->Render(base::TimeDelta(), now, none,
                                        bus_.get());
  if (written <= 0) {
    underruns_.fetch_add(1);
    base::AutoLock scoped(glitch_lock_);
    glitch_info_.total_glitches += 1;
    glitch_info_.total_glitch_duration += params_.buffer_duration();
    std::memset(stream, 0, static_cast<size_t>(len));
    return;
  }
  // Planar float -> interleaved float (AUDIO_F32SYS), the only work on the
  // device thread; scaling already happened inside Render().
  const size_t channels_size = static_cast<size_t>(channels);
  const size_t total = static_cast<size_t>(written) * channels_size;
  for (int c = 0; c < channels; ++c) {
    const float* src = bus_->channel(c);
    for (int i = 0; i < written; ++i) {
      interleaved_[static_cast<size_t>(i) * channels_size +
                   static_cast<size_t>(c)] = src[i];
    }
  }
  std::memcpy(stream, interleaved_.get(), total * sizeof(float));
}

base::scoped_refptr<AudioRendererSink> Sdl2AudioSinkFactory::Create() {
  return base::MakeRefCounted<Sdl2AudioSink>();
}

}  // namespace avbase::media
