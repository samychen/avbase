// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/audio_convert.h"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "media/ffmpeg/av_includes.h"

namespace avbase::media::ffmpeg {
namespace {

constexpr int kInRate = 48000;
constexpr int kOutRate = 44100;
constexpr int kSamples = 4800;  // 100 ms at 48k
constexpr double kTwoPi = 2.0 * 3.14159265358979323846;

// Builds a planar-float input frame filled with a 1 kHz sine so the output is
// non-trivial (resampling actually ran, not a zeroed buffer).
FramePtr MakeInputFrame(int nb_samples, int rate, int channels,
                        AVSampleFormat fmt) {
  FramePtr frame(av_frame_alloc());
  frame->format = fmt;
  frame->sample_rate = rate;
  av_channel_layout_default(&frame->ch_layout, channels);
  frame->nb_samples = nb_samples;
  EXPECT_EQ(av_frame_get_buffer(frame.get(), 0), 0);
  for (int ch = 0; ch < channels; ++ch) {
    float* p = reinterpret_cast<float*>(frame->data[ch]);
    for (int s = 0; s < nb_samples; ++s) {
      p[s] = std::sin(kTwoPi * 1000.0 * s / rate);
    }
  }
  return frame;
}

int TotalSamples(const std::vector<FramePtr>& frames) {
  int n = 0;
  for (const auto& f : frames) {
    n += f->nb_samples;
  }
  return n;
}

TEST(AudioConverterTest, PushWithoutConfigureReturnsEmpty) {
  AudioConverter converter;
  ASSERT_FALSE(converter.Configured());
  FramePtr src = MakeInputFrame(kSamples, kInRate, 1, AV_SAMPLE_FMT_FLTP);
  EXPECT_TRUE(converter.Push(*src).empty());
}

TEST(AudioConverterTest, Resamples48000To44100SampleCount) {
  AudioConverter converter;
  ASSERT_TRUE(converter.Configure(kInRate, 1, AV_SAMPLE_FMT_FLTP, kOutRate, 1,
                                  AV_SAMPLE_FMT_FLTP));
  FramePtr src = MakeInputFrame(kSamples, kInRate, 1, AV_SAMPLE_FMT_FLTP);

  std::vector<FramePtr> out = converter.Push(*src);
  auto tail = converter.Flush();
  for (auto& f : tail) {
    out.push_back(std::move(f));
  }

  // Expected length is N * out/in, rounded by the resampler; allow 1% plus a
  // couple of samples for the resampler's internal delay.
  const int expected = static_cast<int>(
      std::llround(static_cast<double>(kSamples) * kOutRate / kInRate));
  const int total = TotalSamples(out);
  EXPECT_NEAR(total, expected, expected / 100 + 2);

  // Every output frame carries the configured output geometry.
  for (const auto& f : out) {
    EXPECT_EQ(f->sample_rate, kOutRate);
    EXPECT_EQ(f->format, static_cast<int>(AV_SAMPLE_FMT_FLTP));
    EXPECT_EQ(f->ch_layout.nb_channels, 1);
    EXPECT_GT(f->nb_samples, 0);
  }

  // The signal did not collapse to silence: some energy survived resampling.
  double energy = 0.0;
  for (const auto& f : out) {
    const float* p = reinterpret_cast<const float*>(f->data[0]);
    for (int s = 0; s < f->nb_samples; ++s) {
      energy += static_cast<double>(p[s]) * static_cast<double>(p[s]);
    }
  }
  EXPECT_GT(energy, 0.0);
}

TEST(AudioConverterTest, ResamplesMonoToStereoAndRate) {
  AudioConverter converter;
  ASSERT_TRUE(converter.Configure(kInRate, 1, AV_SAMPLE_FMT_FLTP, kOutRate, 2,
                                  AV_SAMPLE_FMT_FLTP));
  FramePtr src = MakeInputFrame(kSamples, kInRate, 1, AV_SAMPLE_FMT_FLTP);
  std::vector<FramePtr> out = converter.Push(*src);
  auto tail = converter.Flush();
  for (auto& f : tail) {
    out.push_back(std::move(f));
  }
  const int expected = static_cast<int>(
      std::llround(static_cast<double>(kSamples) * kOutRate / kInRate));
  EXPECT_NEAR(TotalSamples(out), expected, expected / 100 + 2);
  for (const auto& f : out) {
    EXPECT_EQ(f->ch_layout.nb_channels, 2);
    EXPECT_EQ(f->format, static_cast<int>(AV_SAMPLE_FMT_FLTP));
  }
}

TEST(AudioConverterTest, ReconfigureAndReset) {
  AudioConverter converter;
  ASSERT_TRUE(converter.Configure(kInRate, 1, AV_SAMPLE_FMT_FLTP, kOutRate, 1,
                                  AV_SAMPLE_FMT_FLTP));
  EXPECT_TRUE(converter.Configured());
  // Same geometry: no-op, still configured.
  EXPECT_TRUE(converter.Configure(kInRate, 1, AV_SAMPLE_FMT_FLTP, kOutRate, 1,
                                  AV_SAMPLE_FMT_FLTP));
  EXPECT_TRUE(converter.Configured());
  // Different geometry: rebuilds and still configured.
  EXPECT_TRUE(converter.Configure(kInRate, 2, AV_SAMPLE_FMT_FLTP, kOutRate, 2,
                                  AV_SAMPLE_FMT_FLTP));
  EXPECT_TRUE(converter.Configured());
  converter.Reset();
  EXPECT_FALSE(converter.Configured());
  // After reset, Push is inert again.
  FramePtr src = MakeInputFrame(kSamples, kInRate, 2, AV_SAMPLE_FMT_FLTP);
  EXPECT_TRUE(converter.Push(*src).empty());
}

}  // namespace
}  // namespace avbase::media::ffmpeg
