// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/video_encoder_factory.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "media/filters/decoder_selector.h"  // HwCodecFlag, HwCodecMask

namespace avbase::media::ffmpeg {
namespace {

// E5: The software factory supports H.264 and returns a non-hardware
// capability.
TEST(VideoEncoderFactoryTest, SoftwareFactorySupportsH264) {
  auto factory = CreateSoftwareVideoEncoderFactory();
  ASSERT_TRUE(factory);
  const auto cap = factory->GetCapability();
  EXPECT_FALSE(cap.hardware);
  EXPECT_TRUE(factory->SupportsCodec(
      media::VideoCodec::kH264,
      static_cast<media::HwCodecMask>(media::HwCodecFlag::kAll)));
  EXPECT_TRUE(factory->SupportsCodec(
      media::VideoCodec::kMjpeg,
      static_cast<media::HwCodecMask>(media::HwCodecFlag::kAll)));
}

// E5: Selector returns hardware factories first when they support the codec.
TEST(VideoEncoderFactoryTest, SelectorPrioritizesHardware) {
  auto sw = CreateSoftwareVideoEncoderFactory();
  std::vector<base::scoped_refptr<VideoEncoderFactory>> factories;
  factories.push_back(sw);

#if defined(__APPLE__)
  auto hw = CreateVideoToolboxEncoderFactory();
  factories.push_back(hw);
#endif

  const auto selected = SelectVideoEncoder(
      factories, media::VideoCodec::kH264,
      static_cast<media::HwCodecMask>(media::HwCodecFlag::kAll));
  ASSERT_FALSE(selected.empty());
  if (selected.size() > 1) {
    EXPECT_TRUE(selected[0]->GetCapability().hardware);
    EXPECT_FALSE(selected[1]->GetCapability().hardware);
  }
}

// E5: HwCodecMask filters out unwanted codecs for hardware factories.
TEST(VideoEncoderFactoryTest, HwMaskFiltersCodecs) {
  auto sw = CreateSoftwareVideoEncoderFactory();
  const media::HwCodecMask hevc_only =
      static_cast<media::HwCodecMask>(media::HwCodecFlag::kHevc);

  // Software factory ignores hw_mask.
  EXPECT_TRUE(sw->SupportsCodec(media::VideoCodec::kH264, hevc_only));
  EXPECT_TRUE(sw->SupportsCodec(media::VideoCodec::kHevc, hevc_only));

#if defined(__APPLE__)
  auto hw = CreateVideoToolboxEncoderFactory();
  // Hardware factory respects the mask.
  EXPECT_FALSE(hw->SupportsCodec(media::VideoCodec::kH264, hevc_only));
  EXPECT_TRUE(hw->SupportsCodec(media::VideoCodec::kHevc, hevc_only));
#endif
}

// E5: the factory must be able to say WHICH encoder it would use. This is
// the accessor whose absence made selection unusable — a caller told "use
// VideoToolbox" could not learn that it should ask for
// "h264_videotoolbox", because CreateEncoder() takes a codec name.
TEST(VideoEncoderFactoryTest, SoftwareFactoryNamesItsEncoders) {
  auto factory = CreateSoftwareVideoEncoderFactory();
  ASSERT_TRUE(factory);
  EXPECT_EQ(factory->EncoderNameFor(media::VideoCodec::kH264), "libx264");
  EXPECT_EQ(factory->EncoderNameFor(media::VideoCodec::kHevc), "libx265");
  EXPECT_EQ(factory->EncoderNameFor(media::VideoCodec::kVp9), "libvpx-vp9");
  EXPECT_EQ(factory->EncoderNameFor(media::VideoCodec::kMjpeg), "mjpeg");
  // Codecs it cannot encode answer with nothing rather than a best guess.
  EXPECT_TRUE(factory->EncoderNameFor(media::VideoCodec::kAv1).empty());
}

// E5: the platform list ships at least software, and any hardware factory
// present must outrank it. The list is where the #if-per-platform logic
// belongs, so media/ never has to know what this machine is.
TEST(VideoEncoderFactoryTest, PlatformListIsHardwareFirst) {
  const auto factories = CreateVideoEncoderFactories();
  ASSERT_FALSE(factories.empty());
  int saw_hardware = 0;
  int highest_priority = -1;
  for (const auto& f : factories) {
    EXPECT_TRUE(f);
    if (f->GetCapability().hardware) {
      // Hardware has to know its own codec names, else it is decoration.
      EXPECT_FALSE(f->EncoderNameFor(media::VideoCodec::kH264).empty())
          << f->name() << " claims hardware but names no H.264 encoder";
      ++saw_hardware;
    }
    highest_priority = std::max(highest_priority, f->GetCapability().priority);
  }
#if defined(__APPLE__) || (defined(__linux__) && !defined(__APPLE__))
  EXPECT_GT(saw_hardware, 0);
#endif
  const auto selected = SelectVideoEncoder(
      factories, media::VideoCodec::kH264,
      static_cast<media::HwCodecMask>(media::HwCodecFlag::kAvc));
  ASSERT_FALSE(selected.empty());
  if (saw_hardware > 0) {
    EXPECT_TRUE(selected[0]->GetCapability().hardware)
        << "with kAvc allowed, hardware must be tried first";
  }
}

#if defined(__APPLE__)
// E5: VideoToolbox answers with its own encoder names, and admits to having
// nothing for the codecs it cannot encode.
TEST(VideoEncoderFactoryTest, VideoToolboxNamesHardwareEncoders) {
  auto hw = CreateVideoToolboxEncoderFactory();
  EXPECT_EQ(hw->EncoderNameFor(media::VideoCodec::kH264), "h264_videotoolbox");
  EXPECT_EQ(hw->EncoderNameFor(media::VideoCodec::kHevc), "hevc_videotoolbox");
  EXPECT_TRUE(hw->EncoderNameFor(media::VideoCodec::kVp9).empty());
  EXPECT_TRUE(hw->EncoderNameFor(media::VideoCodec::kMjpeg).empty());
}
#endif

}  // namespace
}  // namespace avbase::media::ffmpeg
