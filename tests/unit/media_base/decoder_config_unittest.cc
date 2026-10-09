// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// AudioDecoderConfig validity tests. Pure value-type tests: they run in the
// no-ffmpeg configuration too.
//
// The two predicates look similar and answer different questions, which is the
// whole reason this file exists. Getting them confused is not academic:
//   * IsValidConfig()  -- "can we decode this at all here?"  (codec known)
//   * HasUsableParameters() -- "did the container measure it?" (rate +
//   channels)
// The demuxer may only drop a stream for the second. Dropping for the first
// would make a build with a minimal decoder set silently hide streams it
// merely cannot decode, which is a property of the build, not of the file --
// and the corpus reports exactly that class as SKIPPED rather than failed.

#include "media/base/decoder_config.h"

#include "gtest/gtest.h"

namespace avbase::media {
namespace {

AudioDecoderConfig MakeAudio(AudioCodec codec, int sample_rate, int channels) {
  AudioDecoderConfig config;
  config.codec = codec;
  config.sample_rate = sample_rate;
  config.channels = channels;
  return config;
}

TEST(AudioDecoderConfigTest, MeasuredStreamIsUsableAndValid) {
  const AudioDecoderConfig config = MakeAudio(AudioCodec::kAac, 48000, 2);
  EXPECT_TRUE(config.HasUsableParameters());
  EXPECT_TRUE(config.IsValidConfig());
}

// An MPEG-PS stream libavformat could not measure arrives as 0 channels. This
// is the case that used to abort the audio renderer.
TEST(AudioDecoderConfigTest, UnmeasurableStreamIsNotUsable) {
  EXPECT_FALSE(MakeAudio(AudioCodec::kMp3, 0, 0).HasUsableParameters());
  EXPECT_FALSE(MakeAudio(AudioCodec::kMp3, 48000, 0).HasUsableParameters());
  EXPECT_FALSE(MakeAudio(AudioCodec::kMp3, 0, 2).HasUsableParameters());
}

// The distinction the demuxer's drop rule rests on: a codec this build has no
// decoder for is still a measured, usable stream, so it must be reported (and
// then skipped by the decoder layer) rather than dropped at the demuxer.
TEST(AudioDecoderConfigTest, UnknownCodecWithMeasuredParametersIsStillUsable) {
  const AudioDecoderConfig config = MakeAudio(AudioCodec::kUnknown, 48000, 2);
  EXPECT_FALSE(config.IsValidConfig());
  EXPECT_TRUE(config.HasUsableParameters());
}

}  // namespace
}  // namespace avbase::media
