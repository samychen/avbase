// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// , and not listed in tests/CMakeLists.txt.
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// Written without a compiler available, so it has never been compiled or run.
// The rule for DRAFT files is that they are excluded from every CMake target
// because a file that cannot compile must not be reachable from a build; the
// ninth round registered one such file by mistake and the next `cmake --build`
// would have failed on it. Add this file to a media_filters_unittests
// target the
// first time it compiles, and delete this banner at the same moment.
//
// This is the checklist from docs/07 §3.9, which is also behaviour difference
// Δ17's acceptance test: ijkpp replaces ijkplayer's SoundTouch dependency with
// its own WSOLA, and the only thing that proves the substitution did not change
// how 2x playback sounds is the pitch assertion below. Item 7 of that checklist
// (diff against Chromium's output at -40 dB) is NOT implemented: it needs
// reference recordings, which need a Chromium build, and docs/07 §7 puts that
// infrastructure in M10.
//
// The tolerances are the ones docs/07 §3.9 states, except where noted.
// They
// have never been executed, so treat a first-run failure as "the tolerance or
// the implementation is wrong" rather than assuming the implementation is.

#include "media/filters/audio_renderer_algorithm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/audio_buffer.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/filters/audio_frame_queue.h"
#include "media/filters/wsola_internals.h"

#include "gtest/gtest.h"

namespace ijkpp::media {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kFramesPerBuffer = 1024;

// Defined here rather than taken from <cmath>'s M_PI, for the reason
// recorded in wsola_internals.h: M_PI is a POSIX extension, and this project
// builds as C++20.
constexpr double kTestPi = 3.14159265358979323846;

// 20 ms of audio at 48 kHz, i.e. internal::TimeToFrames(kOlaWindowSize, ...).
// Spelled out because several tolerances are expressed in windows and a reader
// should not have to recompute it.
constexpr int kOlaWindowFrames = 960;

AudioParameters StereoParams() {
  return AudioParameters(ChannelLayout::kStereo, SampleFormat::kF32P,
                         kSampleRate, kFramesPerBuffer);
}

// Builds a planar-float32 AudioBuffer holding |frames| of a |freq_hz| sine.
// Both channels carry the same signal so a per-channel assertion and a summed
// one agree, which keeps the dominant-frequency search unambiguous.
base::scoped_refptr<AudioBuffer> MakeSineBuffer(int frames, double freq_hz,
                                                base::TimeDelta timestamp,
                                                float amplitude = 0.5f) {
  std::vector<float> mono(static_cast<size_t>(frames));
  for (int n = 0; n < frames; ++n) {
    mono[n] = amplitude * static_cast<float>(
                              std::sin(2.0 * kTestPi * freq_hz * n /
                                       kSampleRate));
  }
  std::vector<uint8_t> data(static_cast<size_t>(frames) * kChannels *
                            sizeof(float));
  for (int c = 0; c < kChannels; ++c) {
    std::memcpy(data.data() + static_cast<size_t>(c) * frames * sizeof(float),
                mono.data(), static_cast<size_t>(frames) * sizeof(float));
  }
  const base::TimeDelta duration = base::SecondsD(
      static_cast<double>(frames) / kSampleRate);
  return AudioBuffer::Create(SampleFormat::kF32P, ChannelLayout::kStereo,
                             kChannels, kSampleRate, frames, timestamp,
                             duration, /*serial=*/0, std::move(data));
}

// Collects |frames| of channel 0 from |bus| starting at |offset|.
void AppendChannel(const AudioBus& bus, int offset, int frames,
                   std::vector<float>* out) {
  const float* ch = bus.channel(0) + offset;
  out->insert(out->end(), ch, ch + frames);
}

// Dominant frequency of |x| by direct DFT over a 1 Hz grid in [lo_hz, hi_hz].
// Deliberately naive: a test that needs a fast transform needs a library, and
// the point here is that the answer is computed by something the reader can
// verify in six lines. At 4096 samples and a 200 Hz-wide grid this is ~10^6
// multiply-adds, which is nothing next to the WSOLA run that produced |x|.
double DominantFrequency(const std::vector<float>& x, double lo_hz,
                         double hi_hz) {
  double best_freq = lo_hz;
  double best_magnitude = -1.0;
  for (double f = lo_hz; f <= hi_hz; f += 1.0) {
    const double w = 2.0 * kTestPi * f / kSampleRate;
    double re = 0.0;
    double im = 0.0;
    for (size_t n = 0; n < x.size(); ++n) {
      // The cast is required, not cosmetic: -Wdouble-promotion is in
      // the warning set and the debug preset builds with -Werror.
      const double sample = static_cast<double>(x[n]);
      re += sample * std::cos(w * static_cast<double>(n));
      im -= sample * std::sin(w * static_cast<double>(n));
    }
    const double magnitude = re * re + im * im;
    if (magnitude > best_magnitude) {
      best_magnitude = magnitude;
      best_freq = f;
    }
  }
  return best_freq;
}

class AudioRendererAlgorithmTest : public ::testing::Test {
 protected:
  void SetUp() override { algorithm_.Initialize(StereoParams()); }

  // Feeds |seconds| of a |freq_hz| sine in |frames|-sized buffers.
  void Feed(double seconds, double freq_hz, int frames_per_buffer) {
    const int total = static_cast<int>(seconds * kSampleRate);
    int done = 0;
    while (done < total) {
      const int n = std::min(frames_per_buffer, total - done);
      algorithm_.EnqueueBuffer(MakeSineBuffer(
          n, freq_hz,
          base::SecondsD(static_cast<double>(done) / kSampleRate)));
      done += n;
    }
    algorithm_.MarkEndOfStream();
  }

  // Drains at |rate| until FillBuffer stops producing, into |out|.
  int Drain(double rate, int frames_per_call, std::vector<float>* out) {
    auto bus = AudioBus::Create(kChannels, frames_per_call);
    int total = 0;
    int idle_rounds = 0;
    while (idle_rounds < 4) {
      const int got =
          algorithm_.FillBuffer(bus.get(), 0, frames_per_call, rate);
      if (got <= 0) {
        ++idle_rounds;
        continue;
      }
      idle_rounds = 0;
      AppendChannel(*bus, 0, got, out);
      total += got;
    }
    return total;
  }

  AudioRendererAlgorithm algorithm_;
};

// ---- docs/07 §3.9 item 1: rate 1.0 takes the fast path, sample for sample ---

TEST_F(AudioRendererAlgorithmTest, RateOneIsPassthroughAndSampleExact) {
  const int kFrames = 4096;
  algorithm_.EnqueueBuffer(
      MakeSineBuffer(kFrames, 440.0, base::TimeDelta()));

  auto bus = AudioBus::Create(kChannels, kFrames);
  const int got = algorithm_.FillBuffer(bus.get(), 0, kFrames, 1.0);
  EXPECT_EQ(kFrames, got);
  // Asserted after the call, not before: FillBuffer() chooses the mode, and
  // last_mode_ happens to default to kPassthrough, so checking it first would
  // pass even if ChooseBufferMode() were broken.
  EXPECT_EQ(AudioRendererAlgorithm::FillBufferMode::kPassthrough,
            algorithm_.last_fill_mode());
  EXPECT_EQ(1.0, algorithm_.effective_playback_rate());
  EXPECT_TRUE(algorithm_.is_queue_empty());

  // Compare against the input rather than against a recomputed sine: the
  // assertion is "passthrough does not touch the samples", not "our sine
  // generator matches itself".
  auto expected = MakeSineBuffer(kFrames, 440.0, base::TimeDelta());
  auto reference = AudioBus::Create(kChannels, kFrames);
  expected->ReadFrames(kFrames, 0, reference.get());
  for (int n = 0; n < kFrames; ++n) {
    ASSERT_FLOAT_EQ(reference->channel(0)[n], bus->channel(0)[n]) << "frame "
                                                                 << n;
  }
}

// ---- docs/07 §3.9 items 2 and 3: duration scales with the rate -------------

TEST_F(AudioRendererAlgorithmTest, RateTwoHalvesTheDuration) {
  const double kSeconds = 1.0;
  Feed(kSeconds, 440.0, 4096);
  std::vector<float> out;
  const int rendered = Drain(2.0, kFramesPerBuffer, &out);

  const int expected = static_cast<int>(kSeconds * kSampleRate) / 2;
  // docs/07 §3.9 allows +/- one analysis window. WSOLA also holds back the tail
  // it cannot complete a window for, so the bound is two windows; tighten this
  // to one once the test has actually run and the real slack is known.
  const int tolerance = 2 * kOlaWindowFrames;
  EXPECT_NEAR(expected, rendered, tolerance)
      << "2x should halve the duration; rendered " << rendered << " frames";
}

TEST_F(AudioRendererAlgorithmTest, RateHalfDoublesTheDuration) {
  const double kSeconds = 1.0;
  Feed(kSeconds, 440.0, 4096);
  std::vector<float> out;
  const int rendered = Drain(0.5, kFramesPerBuffer, &out);

  const int expected = static_cast<int>(kSeconds * kSampleRate) * 2;
  const int tolerance = 2 * kOlaWindowFrames;
  EXPECT_NEAR(expected, rendered, tolerance)
      << "0.5x should double the duration; rendered " << rendered << " frames";
}

// ---- docs/07 §3.9 item 4: pitch is unchanged (Δ17's acceptance) -----------

TEST_F(AudioRendererAlgorithmTest, PitchIsPreservedAtTwoX) {
  const double kTone = 440.0;
  Feed(1.0, kTone, 4096);
  std::vector<float> out;
  Drain(2.0, kFramesPerBuffer, &out);

  // Measure well past the first window: the opening samples are the zero-filled
  // ramp-in of the overlap-add chain and carry no pitch information.
  ASSERT_GT(out.size(), static_cast<size_t>(4 * kOlaWindowFrames));
  const std::vector<float> measured(
      out.begin() + 2 * kOlaWindowFrames,
      out.begin() + 2 * kOlaWindowFrames + 4096);
  const double peak = DominantFrequency(measured, kTone * 0.9, kTone * 1.1);
  // +/-2% per docs/07 §3.9. Naive resampling would put this near 880 Hz, so the
  // assertion distinguishes WSOLA from the thing it replaced.
  EXPECT_NEAR(kTone, peak, kTone * 0.02)
      << "2x playback must preserve pitch; dominant frequency was " << peak;
}

TEST_F(AudioRendererAlgorithmTest, PitchIsPreservedAtHalfX) {
  const double kTone = 440.0;
  Feed(0.5, kTone, 4096);
  std::vector<float> out;
  Drain(0.5, kFramesPerBuffer, &out);

  ASSERT_GT(out.size(), static_cast<size_t>(4 * kOlaWindowFrames));
  const std::vector<float> measured(
      out.begin() + 2 * kOlaWindowFrames,
      out.begin() + 2 * kOlaWindowFrames + 4096);
  const double peak = DominantFrequency(measured, kTone * 0.9, kTone * 1.1);
  EXPECT_NEAR(kTone, peak, kTone * 0.02)
      << "0.5x playback must preserve pitch; dominant frequency was " << peak;
}

// ---- docs/07 §3.9 item 5: Flush() resets everything -----------------------

TEST_F(AudioRendererAlgorithmTest, FlushClearsQueueAndWsolaState) {
  Feed(1.0, 440.0, 4096);
  std::vector<float> discard;
  Drain(2.0, kFramesPerBuffer, &discard);
  ASSERT_FALSE(algorithm_.is_queue_empty()) << "the drain should leave a tail";

  algorithm_.FlushBuffers();
  EXPECT_TRUE(algorithm_.is_queue_empty());
  EXPECT_EQ(0, algorithm_.buffered_frames());
  EXPECT_FALSE(algorithm_.front_timestamp().has_value());

  // After a flush the first frames out must come from the newly enqueued audio,
  // not from a half-consumed window left over before the flush.
  algorithm_.EnqueueBuffer(MakeSineBuffer(4096, 440.0, base::TimeDelta()));
  auto bus = AudioBus::Create(kChannels, 1024);
  const int got = algorithm_.FillBuffer(bus.get(), 0, 1024, 1.0);
  EXPECT_EQ(1024, got);
}

// ---- docs/07 §3.9 item 6: no loss or duplication across many calls --------

TEST_F(AudioRendererAlgorithmTest, PassthroughLosesNoFramesAcrossManyCalls) {
  const int kBuffers = 20;
  const int kFramesEach = 512;
  for (int i = 0; i < kBuffers; ++i) {
    algorithm_.EnqueueBuffer(MakeSineBuffer(
        kFramesEach, 440.0,
        base::SecondsD(static_cast<double>(i * kFramesEach) / kSampleRate)));
  }
  auto bus = AudioBus::Create(kChannels, kFramesEach);
  int total = 0;
  for (int i = 0; i < kBuffers; ++i) {
    total += algorithm_.FillBuffer(bus.get(), 0, kFramesEach, 1.0);
  }
  EXPECT_EQ(kBuffers * kFramesEach, total);
  EXPECT_TRUE(algorithm_.is_queue_empty());
}

// ---- underflow behaviour: report a short count, do not invent silence ------

TEST_F(AudioRendererAlgorithmTest, UnderflowReturnsFewerFramesNotSilence) {
  algorithm_.EnqueueBuffer(MakeSineBuffer(256, 440.0, base::TimeDelta()));
  auto bus = AudioBus::Create(kChannels, 1024);
  const int got = algorithm_.FillBuffer(bus.get(), 0, 1024, 1.0);
  // The caller turns a short count into an AudioGlitchInfo entry. Padding here
  // instead would make underruns unmeasurable, which is the whole reason
  // audio_glitches exists in PlaybackStats.
  EXPECT_EQ(256, got);
}

TEST_F(AudioRendererAlgorithmTest, ZeroRateAndZeroRequestAreNoOps) {
  algorithm_.EnqueueBuffer(MakeSineBuffer(1024, 440.0, base::TimeDelta()));
  auto bus = AudioBus::Create(kChannels, 1024);
  EXPECT_EQ(0, algorithm_.FillBuffer(bus.get(), 0, 1024, 0.0));
  EXPECT_EQ(0, algorithm_.FillBuffer(bus.get(), 0, 0, 1.0));
  EXPECT_EQ(1024, algorithm_.buffered_frames());
}

// ---- queue sizing policy (consumed by BufferController at M9) -------------

TEST_F(AudioRendererAlgorithmTest, PlaybackThresholdDoublesUpToTheCap) {
  const int initial = algorithm_.queue_playback_threshold();
  const int capacity = algorithm_.queue_capacity();
  EXPECT_EQ(initial, capacity);

  algorithm_.IncreasePlaybackThreshold();
  const int doubled = algorithm_.queue_playback_threshold();
  if (doubled != initial) {          // unless it was already at the cap
    EXPECT_EQ(2 * initial, doubled);
    EXPECT_EQ(doubled, algorithm_.queue_capacity());
  }
  for (int i = 0; i < 20; ++i) {
    algorithm_.IncreasePlaybackThreshold();
  }
  // 3 s of 48 kHz stereo is the ceiling (kMaxCapacity), and doubling must stop
  // there rather than growing without bound.
  EXPECT_LE(algorithm_.queue_capacity(), 3 * kSampleRate);
  EXPECT_FALSE(algorithm_.IsQueueAdequateForPlayback());
}

TEST_F(AudioRendererAlgorithmTest, LatencyHintIsClampedAtBothEnds) {
  algorithm_.SetLatencyHint(base::Seconds(60));
  EXPECT_LE(algorithm_.queue_playback_threshold(), 3 * kSampleRate);

  algorithm_.SetLatencyHint(base::Microseconds(1));
  EXPECT_GE(algorithm_.queue_playback_threshold(), 2 * kFramesPerBuffer);

  algorithm_.SetLatencyHint(base::Milliseconds(500));
  EXPECT_NEAR(500 * kSampleRate / 1000,
              algorithm_.queue_playback_threshold(), 2);

  // Clearing the hint restores the default instead of keeping the last value.
  algorithm_.SetLatencyHint(std::nullopt);
  EXPECT_NEAR(200 * kSampleRate / 1000,
              algorithm_.queue_playback_threshold(), 2);
}

TEST_F(AudioRendererAlgorithmTest, BufferedDurationMatchesBufferedFrames) {
  algorithm_.EnqueueBuffer(MakeSineBuffer(kSampleRate, 440.0,
                                          base::TimeDelta()));
  EXPECT_EQ(kSampleRate, algorithm_.buffered_frames());
  EXPECT_EQ(base::Seconds(1), algorithm_.buffered_duration());
}

// ---- AudioFrameQueue, which WSOLA's peek-then-drop pattern depends on -----

TEST(AudioFrameQueueTest, PeekDoesNotConsumeButReadDoes) {
  AudioFrameQueue queue;
  auto scratch = AudioBus::Create(kChannels, 512);
  queue.Append(MakeSineBuffer(512, 440.0, base::TimeDelta()));
  ASSERT_EQ(512, queue.frames());

  auto bus = AudioBus::Create(kChannels, 512);
  EXPECT_EQ(512, queue.PeekFrames(512, 0, 0, bus.get(), scratch.get()));
  EXPECT_EQ(512, queue.frames()) << "peek must not consume";

  EXPECT_EQ(256, queue.ReadFrames(256, 0, bus.get(), scratch.get()));
  EXPECT_EQ(256, queue.frames()) << "read must consume what it returned";
}

TEST(AudioFrameQueueTest, PeekAtAnOffsetSeesLaterFrames) {
  AudioFrameQueue queue;
  auto scratch = AudioBus::Create(kChannels, 512);
  queue.Append(MakeSineBuffer(512, 440.0, base::TimeDelta()));

  auto at_zero = AudioBus::Create(kChannels, 64);
  auto at_256 = AudioBus::Create(kChannels, 64);
  queue.PeekFrames(64, 0, 0, at_zero.get(), scratch.get());
  queue.PeekFrames(64, 256, 0, at_256.get(), scratch.get());
  EXPECT_NE(at_zero->channel(0)[1], at_256->channel(0)[1])
      << "a 440 Hz sine differs between frame 1 and frame 257";
}

TEST(AudioFrameQueueTest, TailPastTheEndIsZeroFilled) {
  AudioFrameQueue queue;
  auto scratch = AudioBus::Create(kChannels, 512);
  queue.Append(MakeSineBuffer(100, 440.0, base::TimeDelta()));

  auto bus = AudioBus::Create(kChannels, 256);
  bus->channel(0)[200] = 1.0f;         // stale value that must be overwritten
  const int got = queue.PeekFrames(256, 0, 0, bus.get(), scratch.get());
  EXPECT_EQ(100, got);
  for (int n = 100; n < 256; ++n) {
    EXPECT_FLOAT_EQ(0.0f, bus->channel(0)[n]) << "frame " << n;
    EXPECT_FLOAT_EQ(0.0f, bus->channel(1)[n]) << "frame " << n;
  }
}

TEST(AudioFrameQueueTest, SeekFramesDropsAcrossBufferBoundaries) {
  AudioFrameQueue queue;
  queue.Append(MakeSineBuffer(300, 440.0, base::TimeDelta()));
  queue.Append(MakeSineBuffer(300, 440.0, base::Milliseconds(6250)));
  ASSERT_EQ(600, queue.frames());

  queue.SeekFrames(350);               // crosses from buffer 0 into buffer 1
  EXPECT_EQ(250, queue.frames());

  auto scratch = AudioBus::Create(kChannels, 256);
  auto bus = AudioBus::Create(kChannels, 256);
  EXPECT_EQ(250, queue.PeekFrames(250, 0, 0, bus.get(), scratch.get()));
  EXPECT_TRUE(queue.FrontTimestamp().has_value());
}

TEST(AudioFrameQueueTest, EndOfStreamMarkerIsRejected) {
  AudioFrameQueue queue;
  queue.Append(AudioBuffer::CreateEOSBuffer());
  EXPECT_EQ(0, queue.frames())
      << "an EOS marker carries no audio; counting it would make frames() and "
         "front_timestamp() lie to the buffering controller";
  EXPECT_TRUE(queue.empty());
}

}  // namespace
}  // namespace ijkpp::media
