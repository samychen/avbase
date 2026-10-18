// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The hardware decoder factory's per-stream gate. A device may accept a stream
// at avcodec_open2() and then fail on the first packet -- long after
// Initialize() reported success and the fallback chain moved on. VideoToolbox
// does exactly that with 4:2:2 / 4:4:4 H.264. The gate
// (FFmpegHwDecoderSpec::config_supported) must be consulted in
// CreateVideoDecoder and turn a "not me" into a nullptr, so the software tail
// in the same candidate list stays reachable -- see the comment on
// VideotoolboxSupportsConfig.

#include "media/ffmpeg/ffmpeg_hw_decoder_factory.h"

#include "base/memory/ref_counted.h"
#include "base/test/task_environment.h"
#include "media/base/decoder_config.h"
#include "media/ffmpeg/ffmpeg_hw_video_decoder.h"
#include "platform/hwaccel/videotoolbox_hw_spec.h"

#include "gtest/gtest.h"

namespace avbase::media::ffmpeg {
namespace {

media::VideoDecoderConfig H264Config(const std::string& profile) {
  media::VideoDecoderConfig config;
  config.codec = media::VideoCodec::kH264;
  config.codec_name = "h264";
  config.profile = profile;
  config.coded_size = media::Size{1280, 720};
  config.natural_size = config.coded_size;
  return config;
}

media::VideoDecoderConfig HevcConfig() {
  media::VideoDecoderConfig config;
  config.codec = media::VideoCodec::kHevc;
  config.codec_name = "hevc";
  config.coded_size = media::Size{1280, 720};
  config.natural_size = config.coded_size;
  return config;
}

// VideoToolbox declines a 4:4:4 / 4:2:2 H.264 stream before it ever builds a
// decoder, so the software decoder behind it is still available. The 4:2:0 and
// unreported cases are accepted (benefit of the doubt), and non-H264 codecs
// bypass the chroma gate entirely.
TEST(FFmpegHwDecoderFactoryTest, VideotoolboxDeclinesUnsupportedChroma) {
  base::test::TaskEnvironment env;
  auto runner = base::SequencedTaskRunner::GetCurrentDefault();
  media::ffmpeg::FFmpegHwDecoderSpec spec =
      avbase::platform::hwaccel::VideotoolboxHwSpec();
  ASSERT_TRUE(spec.config_supported)
      << "VideotoolboxHwSpec must wire the chroma gate";
  auto factory = base::MakeRefCounted<FFmpegHwVideoDecoderFactory>(
      spec, runner, static_cast<HwCodecMask>(HwCodecFlag::kAll));

  // High 4:4:4 Predictive (profile_idc 244): the measured failure.
  EXPECT_EQ(factory->CreateVideoDecoder(H264Config("244")), nullptr)
      << "4:4:4 H.264 must be declined by VideoToolbox";
  // High 4:2:2 Predictive (122) is likewise unsupported.
  EXPECT_EQ(factory->CreateVideoDecoder(H264Config("122")), nullptr)
      << "4:2:2 H.264 must be declined by VideoToolbox";
  // High (100) and Constrained Baseline (66) play.
  EXPECT_NE(factory->CreateVideoDecoder(H264Config("100")), nullptr)
      << "4:2:0 H.264 High must be accepted";
  EXPECT_NE(factory->CreateVideoDecoder(H264Config("66")), nullptr)
      << "Constrained Baseline must be accepted";
  // No reported profile: give the hardware the benefit of the doubt.
  EXPECT_NE(factory->CreateVideoDecoder(H264Config("")), nullptr)
      << "an unreported profile must not be refused";
  // HEVC carries no measured chroma limit on this device.
  EXPECT_NE(factory->CreateVideoDecoder(HevcConfig()), nullptr)
      << "HEVC must bypass the H.264 chroma gate";
}

// An empty config_supported means "the codec list is the only filter": a
// stream the device cannot actually decode still reaches a decoder here, so the
// existing first-packet failure path (and its 20-error fallback) is what
// decides -- not a premature decline that would strand the player on kUnknown.
TEST(FFmpegHwDecoderFactoryTest, EmptyGateIsNoFilter) {
  base::test::TaskEnvironment env;
  auto runner = base::SequencedTaskRunner::GetCurrentDefault();
  media::ffmpeg::FFmpegHwDecoderSpec spec;
  spec.device_type = "videotoolbox";
  spec.codecs = {media::VideoCodec::kH264};
  spec.decoder_type = VideoDecoderType::kVideoToolbox;
  spec.display_name = "NoGate";
  // config_supported intentionally left empty.
  auto factory = base::MakeRefCounted<FFmpegHwVideoDecoderFactory>(
      spec, runner, static_cast<HwCodecMask>(HwCodecFlag::kAll));
  EXPECT_FALSE(spec.config_supported);
  EXPECT_NE(factory->CreateVideoDecoder(H264Config("244")), nullptr)
      << "an empty gate must not decline anything";
}

}  // namespace
}  // namespace avbase::media::ffmpeg
