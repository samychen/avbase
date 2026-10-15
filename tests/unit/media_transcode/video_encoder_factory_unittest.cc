// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/video_encoder_factory.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "avbase/BuildConfig.h"
#include "gtest/gtest.h"
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

#if AVBASE_ENABLE_VIDEOTOOLBOX
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

#if AVBASE_ENABLE_VIDEOTOOLBOX
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
// present must outrank it. Selection is expressed in AVBASE_ENABLE_* build
// flags rather than host macros (video_encoder_factory.h records why the
// list is not split the way the decoder side is), which makes the list's
// composition a property of the BUILD — asserted here against those same
// flags: a flag that is on must have its factory listed, and NVENC,
// deliberately unguarded because it is just an FFmpeg encoder name, must be
// listed on every build. Deleting a push_back in CreateVideoEncoderFactories()
// therefore turns this red instead of silently shrinking the list.
TEST(VideoEncoderFactoryTest, PlatformListIsHardwareFirst) {
  const auto factories = CreateVideoEncoderFactories();
  ASSERT_FALSE(factories.empty());
  auto listed = [&factories](const char* factory_name) {
    for (const auto& f : factories) {
      if (f && std::string(f->name()) == factory_name) {
        return true;
      }
    }
    return false;
  };
  int saw_hardware = 0;
  int highest_priority = -1;
  for (const auto& f : factories) {
    ASSERT_TRUE(f);
    if (f->GetCapability().hardware) {
      // Hardware has to know its own codec names, else it is decoration.
      EXPECT_FALSE(f->EncoderNameFor(media::VideoCodec::kH264).empty())
          << f->name() << " claims hardware but names no H.264 encoder";
      ++saw_hardware;
    }
    highest_priority = std::max(highest_priority, f->GetCapability().priority);
  }
  EXPECT_TRUE(listed("NVENC"))
      << "the NVENC factory is unguarded by design and must be listed";
  // NVENC is a hardware factory, so this holds on every build — it used to be
  // guarded by "is this Apple or Linux", which stopped meaning anything once
  // the list followed the build flags instead of the host macros.
  EXPECT_GT(saw_hardware, 0);
#if AVBASE_ENABLE_VIDEOTOOLBOX
  EXPECT_TRUE(listed("VideoToolbox"))
      << "AVBASE_ENABLE_VIDEOTOOLBOX is on but the list has no VideoToolbox "
         "factory";
#endif
#if AVBASE_ENABLE_VAAPI
  EXPECT_TRUE(listed("VAAPI"))
      << "AVBASE_ENABLE_VAAPI is on but the list has no VAAPI factory";
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

#if AVBASE_ENABLE_VIDEOTOOLBOX
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
