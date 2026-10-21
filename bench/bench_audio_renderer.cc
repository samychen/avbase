// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The docs/04 §8 entry `AudioRendererAlgorithm::FillBuffer()` (budget
// < 60 us per 1024 frames at 48 kHz stereo, rate 1.0 = the pure-copy
// passthrough path). It sat in that document's "未落地" list with the note
// "需要音频参数与时间轴夹具" -- the fixtures are three constructors
// and a sine, all of which live in media/base, so nothing here needs a
// decoder, a device or a thread.
//
// WHAT IS DELIBERATELY NOT MEASURED: the WSOLA path (rate != 1.0). Its cost
// is documented as arithmetic in audio_renderer_algorithm.h -- one
// GetOptimalBlock() scores 1440 candidates x 960 frames x 2 channels, ~18 ms
// per 1024 output frames, which is why the class pre-stretches on S4 instead
// of running it on the device thread. This harness cannot time it honestly:
// --iters is a FLOOR that only scales up, so a single 18 ms operation would
// make the probe round alone run for minutes. The WSOLA number belongs to a
// harness with a per-benchmark iteration cap, not to this one.
//
// Nor is `BM_AudioRenderCallback` here: that is the device-thread Render()
// with a populated ring, which needs AudioRendererImpl plus the fake decoder
// and fake sink from tests/support -- and this target is configured with
// AVBASE_BUILD_TESTS=OFF by both gate_local and CI (no test support target
// exists there). Duplicating the fakes into bench/ would drift from the
// copies the unit tests use. The entry stays open in docs/04 §8 with that
// reason recorded.

#include "bench.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/audio_buffer.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/filters/audio_renderer_algorithm.h"

namespace {

using avbase::base::scoped_refptr;
using avbase::base::TimeDelta;
using avbase::media::AudioBuffer;
using avbase::media::AudioBus;
using avbase::media::AudioParameters;
using avbase::media::AudioRendererAlgorithm;
using avbase::media::ChannelLayout;
using avbase::media::SampleFormat;

// The reference configuration from docs/04 §8: 48 kHz stereo, planar float.
constexpr int kChannels = 2;
constexpr int kSampleRate = 48000;
// The budget's unit of work: one device period worth of frames.
constexpr int kFramesPerCall = 1024;
// 440 Hz at 0.5 amplitude: a real signal, far from the denormal range where
// some CPUs take a slow path per sample.
constexpr double kSineHz = 440.0;
constexpr float kSineAmplitude = 0.5f;
// Defined here rather than taken from <cmath>'s M_PI: M_PI is a POSIX
// extension, and this project does not rely on it (see wsola_internals.h).
constexpr double kPi = 3.14159265358979323846;
// Pool of pre-built buffers cycled by the steady-state loop. Building a
// buffer is 1024 sin() calls (~20 us), an order of magnitude more than the
// copy being measured, so it must not happen inside the timed region.
constexpr int kPoolBuffers = 64;

AudioParameters StereoParams() {
  return AudioParameters(ChannelLayout::kStereo, SampleFormat::kF32P,
                         kSampleRate, kFramesPerCall);
}

// |frames| of phase-continuous sine from |start_sample|, wrapped into a
// planar-float32 stereo buffer. Phase continuity is what the unit-test
// fixtures use and it costs nothing here; both channels carry the same
// signal because nothing in the passthrough path reads them differently.
scoped_refptr<AudioBuffer> MakeSineBuffer(int frames, int64_t start_sample) {
  std::vector<uint8_t> data(static_cast<size_t>(frames) *
                            static_cast<size_t>(kChannels) * sizeof(float));
  auto* planar = reinterpret_cast<float*>(data.data());
  for (int n = 0; n < frames; ++n) {
    const double phase = 2.0 * kPi * kSineHz *
                         static_cast<double>(start_sample + n) /
                         static_cast<double>(kSampleRate);
    const float sample = kSineAmplitude * static_cast<float>(std::sin(phase));
    for (int c = 0; c < kChannels; ++c) {
      planar[static_cast<size_t>(c) * static_cast<size_t>(frames) +
             static_cast<size_t>(n)] = sample;
    }
  }
  return AudioBuffer::Create(
      SampleFormat::kF32P, ChannelLayout::kStereo, kChannels, kSampleRate,
      frames,
      TimeDelta::FromSecondsD(static_cast<double>(start_sample) /
                              static_cast<double>(kSampleRate)),
      TimeDelta::FromSecondsD(static_cast<double>(frames) /
                              static_cast<double>(kSampleRate)),
      /*serial=*/0, std::move(data));
}

AVBASE_BENCH(BM_AudioRendererAlgorithm_FillBuffer) {
  AudioRendererAlgorithm algorithm;
  algorithm.Initialize(StereoParams());
  auto dest = AudioBus::Create(kChannels, kFramesPerCall);

  // Steady state, built once: the queue starts deep enough that the first
  // FillBuffer cannot starve, and the loop below keeps the depth flat by
  // appending exactly one buffer for each one consumed. A starved call
  // returns 0 frames and would time the dry path instead of the copy.
  std::vector<scoped_refptr<AudioBuffer>> pool;
  pool.reserve(kPoolBuffers);
  int64_t start_sample = 0;
  for (int i = 0; i < kPoolBuffers; ++i) {
    pool.push_back(MakeSineBuffer(kFramesPerCall, start_sample));
    start_sample += kFramesPerCall;
    algorithm.EnqueueBuffer(pool.back());
  }
  // Depth below which the loop tops the queue back up. One buffer is
  // consumed per iteration, so this fires once per iteration in the worst
  // case; the cost is one refptr copy plus a push_back, and it is part of
  // the steady state a real decoder/device pair runs in, not overhead
  // smuggled into the measurement.
  constexpr int kRefillBelowFrames = kFramesPerCall;

  uint64_t sink = 0;
  size_t pool_next = 0;
  for (uint64_t i = 0; i < iters; ++i) {
    if (algorithm.buffered_frames() < kRefillBelowFrames) {
      algorithm.EnqueueBuffer(pool[pool_next]);
      pool_next = (pool_next + 1) % pool.size();
    }
    const int rendered =
        algorithm.FillBuffer(dest.get(), 0, kFramesPerCall, 1.0);
    sink += static_cast<uint64_t>(rendered);
  }
  avbase::bench::Sink(sink);
}

}  // namespace
