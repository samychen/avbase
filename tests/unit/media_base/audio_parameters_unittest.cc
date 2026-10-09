// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// AudioParameters validity and channel-count tests. These are pure value-type
// tests, so they run in the no-ffmpeg configuration too.
//
// They exist because a wrong answer here is not a wrong picture, it is a
// process abort: RendererImpl builds the audio renderer's parameters from the
// demuxer's AudioDecoderConfig, and AudioRendererAlgorithm::Initialize() CHECKs
// is_valid() on them. Two perfectly playable inputs used to fail that CHECK:
//
//   * a container that does not declare the audio sample format (a bare .ac3,
//     an Ogg/Vorbis, an MPEG-PS with mp2), which libavformat reports as
//     AV_SAMPLE_FMT_NONE and the demuxer as SampleFormat::kUnknown;
//   * any channel count without a named layout (3, 4, 5, 7), which the demuxer
//     labels ChannelLayout::kDiscrete.
//
// Both are covered below, and both are the kind of case that is easy to keep
// fixed only because a test names it.

#include "media/base/audio_parameters.h"

#include "gtest/gtest.h"

namespace avbase::media {
namespace {

// The named-layout path must keep working: existing callers pass a layout and
// no count, and stats/telemetry read channels() back.
TEST(AudioParametersTest, NamedLayoutDerivesChannelCount) {
  const auto channels = [](ChannelLayout layout) {
    return AudioParameters(layout, SampleFormat::kS16, 48000, 1024).channels();
  };
  EXPECT_EQ(channels(ChannelLayout::kMono), 1);
  EXPECT_EQ(channels(ChannelLayout::kStereo), 2);
  EXPECT_EQ(channels(ChannelLayout::k5_1), 6);
  EXPECT_EQ(channels(ChannelLayout::k7_1), 8);
}

// The regression. kUnknown used to make is_valid() false, so a raw AC3 or an
// Ogg/Vorbis file aborted the player before it produced a single frame.
TEST(AudioParametersTest, UnknownSampleFormatIsStillValid) {
  const AudioParameters params(ChannelLayout::kStereo, SampleFormat::kUnknown,
                               48000, 1024);
  EXPECT_TRUE(params.is_valid());
  EXPECT_EQ(params.channels(), 2);
  EXPECT_EQ(params.sample_rate(), 48000);
}

// The other regression: kDiscrete carries no count of its own, so the explicit
// count is what makes a 4-channel stream describable at all.
TEST(AudioParametersTest, DiscreteLayoutUsesTheExplicitChannelCount) {
  for (int channels : {3, 4, 5, 7}) {
    const AudioParameters params(ChannelLayout::kDiscrete, SampleFormat::kF32P,
                                 48000, 1024, channels);
    EXPECT_EQ(params.channels(), channels);
    EXPECT_TRUE(params.is_valid()) << "channels=" << channels;
  }
}

// Without an explicit count kDiscrete is still 0 channels -- the honest answer,
// and an invalid one. This is the state that must fail rather than pretend, so
// the assertion pins it; nothing upstream may build parameters this way.
TEST(AudioParametersTest, DiscreteLayoutWithoutACountIsInvalid) {
  const AudioParameters params(ChannelLayout::kDiscrete, SampleFormat::kF32P,
                               48000, 1024);
  EXPECT_EQ(params.channels(), 0);
  EXPECT_FALSE(params.is_valid());
}

TEST(AudioParametersTest, ZeroRateOrZeroFramesIsInvalid) {
  EXPECT_FALSE(
      AudioParameters(ChannelLayout::kStereo, SampleFormat::kF32P, 0, 1024)
          .is_valid());
  EXPECT_FALSE(
      AudioParameters(ChannelLayout::kStereo, SampleFormat::kF32P, 48000, 0)
          .is_valid());
}

TEST(AudioParametersTest, BufferDurationFollowsRateAndFrames) {
  const AudioParameters params(ChannelLayout::kStereo, SampleFormat::kF32P,
                               48000, 480);
  EXPECT_EQ(params.buffer_duration(), base::Milliseconds(10));
}

}  // namespace
}  // namespace avbase::media
