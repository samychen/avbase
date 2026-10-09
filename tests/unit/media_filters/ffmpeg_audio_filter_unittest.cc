// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/ffmpeg_audio_filter.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace avbase::media {
namespace {

constexpr int kRate = 44100;
constexpr int kChannels = 2;
constexpr int kFrames = 4096;

// A 440 Hz full-scale sine, planar float (the decode path's output format).
base::scoped_refptr<AudioBuffer> MakeSineBuffer(base::TimeDelta ts,
                                                int64_t* sample_cursor) {
  std::vector<uint8_t> data(kFrames * kChannels * sizeof(float));
  auto* planes = reinterpret_cast<float*>(data.data());
  for (int ch = 0; ch < kChannels; ++ch) {
    for (int i = 0; i < kFrames; ++i) {
      const int64_t n = *sample_cursor + i;
      planes[ch * kFrames + i] = static_cast<float>(std::sin(
          2.0 * 3.14159265358979 * 440.0 * static_cast<double>(n) / kRate));
    }
  }
  *sample_cursor += kFrames;
  return AudioBuffer::Create(
      SampleFormat::kF32P,
      kChannels == 1 ? ChannelLayout::kMono : ChannelLayout::kStereo, kChannels,
      kRate, kFrames, ts, base::SecondsD(static_cast<double>(kFrames) / kRate),
      0, std::move(data));
}

double PeakAmplitude(const AudioBuffer& buffer) {
  const auto* floats = reinterpret_cast<const float*>(buffer.data().data());
  const size_t count = buffer.data().size() / sizeof(float);
  double peak = 0.0;
  for (size_t i = 0; i < count; ++i) {
    peak = std::max(peak, std::abs(static_cast<double>(floats[i])));
  }
  return peak;
}

TEST(AudioFilterTest, VolumeGraphHalvesTheAmplitude) {
  FFmpegAudioFilter filter;
  ASSERT_TRUE(filter.Initialize("volume=0.5", kRate, kChannels, kFrames));

  int64_t cursor = 0;
  auto in = MakeSineBuffer(base::TimeDelta(), &cursor);
  std::vector<base::scoped_refptr<AudioBuffer>> out;
  ASSERT_TRUE(filter.Process(in, &out));
  ASSERT_FALSE(out.empty());
  EXPECT_GT(out.size(), 0u);

  double peak = 0.0;
  int64_t frames = 0;
  for (const auto& buffer : out) {
    if (buffer->end_of_stream()) {
      continue;
    }
    peak = std::max(peak, PeakAmplitude(*buffer));
    frames += buffer->frame_count();
  }
  // The graph halves the amplitude; the filter chain may delay a few samples
  // (volume is memoryless, but allow slack for the graph's internal
  // buffering), so compare peaks and require most samples through.
  EXPECT_NEAR(peak, 0.5, 0.05);
  EXPECT_GT(frames, kFrames / 2);
  EXPECT_LE(frames, kFrames + 4096);
}

TEST(AudioFilterTest, EosFlushesAndPropagates) {
  FFmpegAudioFilter filter;
  ASSERT_TRUE(filter.Initialize("volume=1.0", kRate, kChannels, kFrames));

  int64_t cursor = 0;
  auto in = MakeSineBuffer(base::TimeDelta(), &cursor);
  std::vector<base::scoped_refptr<AudioBuffer>> out;
  ASSERT_TRUE(filter.Process(in, &out));
  const size_t after_input = out.size();

  // EOS: the graph must flush and hand an EOS buffer back.
  ASSERT_TRUE(filter.Process(AudioBuffer::CreateEOSBuffer(), &out));
  ASSERT_GT(out.size(), after_input);
  EXPECT_TRUE(out.back()->end_of_stream());
}

TEST(AudioFilterTest, InvalidGraphFailsToInitialize) {
  FFmpegAudioFilter filter;
  EXPECT_FALSE(filter.Initialize("this_is_not_a_filter_name", kRate, 2, 1024));
}

}  // namespace
}  // namespace avbase::media
