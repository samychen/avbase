// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// AudioBuffer layout and conversion tests. These are pure value-type tests:
// they run in the no-ffmpeg configuration too, which is where the byte-layout
// arithmetic is most likely to be wrong and hardest to notice later.

#include "media/base/audio_buffer.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"
#include "media/base/audio_bus.h"

namespace avbase::media {
namespace {

base::scoped_refptr<AudioBuffer> MakeInterleavedS16(int frames) {
  // L/R ramp: left = +i, right = -i, so a swapped or mis-strided read is
  // immediately visible.
  std::vector<uint8_t> data(static_cast<size_t>(frames) * 4);
  for (int i = 0; i < frames; ++i) {
    const int16_t left = static_cast<int16_t>(i * 100);
    const int16_t right = static_cast<int16_t>(-i * 100);
    std::memcpy(data.data() + i * 4, &left, 2);
    std::memcpy(data.data() + i * 4 + 2, &right, 2);
  }
  return AudioBuffer::Create(SampleFormat::kS16, ChannelLayout::kStereo, 2,
                             48000, frames, base::Microseconds(1000),
                             base::Microseconds(frames * 1000000 / 48000),
                             /*serial=*/7, std::move(data));
}

base::scoped_refptr<AudioBuffer> MakePlanarF32P(int frames) {
  // Channel 0 holds +1.0, channel 1 holds -1.0 for every frame. Planar layouts
  // are where a linesize-vs-nb_samples mistake shows up as channel bleed.
  std::vector<uint8_t> data(static_cast<size_t>(frames) * 4 * 2);
  float* p0 = reinterpret_cast<float*>(data.data());
  float* p1 = reinterpret_cast<float*>(data.data() + frames * 4);
  for (int i = 0; i < frames; ++i) {
    p0[i] = 1.0f;
    p1[i] = -1.0f;
  }
  return AudioBuffer::Create(SampleFormat::kF32P, ChannelLayout::kStereo, 2,
                             44100, frames, base::TimeDelta(),
                             base::Microseconds(frames * 1000000 / 44100),
                             /*serial=*/1, std::move(data));
}

TEST(AudioBufferTest, InterleavedS16ReadsBothChannelsWithCorrectStride) {
  auto buffer = MakeInterleavedS16(/*frames=*/8);
  ASSERT_FALSE(buffer->is_planar());
  EXPECT_EQ(8, buffer->frame_count());
  EXPECT_EQ(7, buffer->serial());

  auto bus = AudioBus::Create(2, 8);
  const int read = buffer->ReadFrames(8, 0, bus.get());
  EXPECT_EQ(8, read);
  for (int i = 0; i < 8; ++i) {
    EXPECT_NEAR(static_cast<float>(i * 100) / 32768.0f, bus->channel(0)[i],
                1e-6f)
        << "frame " << i << " left";
    EXPECT_NEAR(static_cast<float>(-i * 100) / 32768.0f, bus->channel(1)[i],
                1e-6f)
        << "frame " << i << " right";
  }
}

TEST(AudioBufferTest, PlanarF32KeepsChannelsSeparate) {
  auto buffer = MakePlanarF32P(/*frames=*/16);
  ASSERT_TRUE(buffer->is_planar());

  auto bus = AudioBus::Create(2, 16);
  EXPECT_EQ(16, buffer->ReadFrames(16, 0, bus.get()));
  for (int i = 0; i < 16; ++i) {
    EXPECT_FLOAT_EQ(1.0f, bus->channel(0)[i]) << "frame " << i;
    EXPECT_FLOAT_EQ(-1.0f, bus->channel(1)[i]) << "frame " << i;
  }
}

TEST(AudioBufferTest, PlanarChannelDataSlicesAreDisjoint) {
  auto buffer = MakePlanarF32P(/*frames=*/4);
  const std::span<const uint8_t> ch0 = buffer->channel_data(0);
  const std::span<const uint8_t> ch1 = buffer->channel_data(1);
  ASSERT_EQ(16u, ch0.size());  // 4 frames * 4 bytes
  ASSERT_EQ(16u, ch1.size());
  EXPECT_NE(ch0.data(), ch1.data());
  EXPECT_EQ(ch0.data() + 16, ch1.data());
  // Out-of-range channels must be empty, not a wild slice.
  EXPECT_TRUE(buffer->channel_data(2).empty());
  EXPECT_TRUE(buffer->channel_data(-1).empty());
}

TEST(AudioBufferTest, ReadFramesHonoursOffsetAndClampsToAvailable) {
  auto buffer = MakeInterleavedS16(/*frames=*/8);
  auto bus = AudioBus::Create(2, 4);
  // Read 4 frames starting at frame 4.
  EXPECT_EQ(4, buffer->ReadFrames(4, 4, bus.get()));
  EXPECT_NEAR(400.0f / 32768.0f, bus->channel(0)[0], 1e-6f);
  // Asking for more than remains returns only what is there.
  EXPECT_EQ(2, buffer->ReadFrames(99, 6, bus.get()));
  // An offset past the end yields nothing rather than reading out of bounds.
  EXPECT_EQ(0, buffer->ReadFrames(4, 100, bus.get()));
}

TEST(AudioBufferTest, ReadFramesIntoNarrowerBusOnlyFillsAvailableChannels) {
  auto buffer = MakeInterleavedS16(/*frames=*/4);
  auto mono = AudioBus::Create(1, 4);
  EXPECT_EQ(4, buffer->ReadFrames(4, 0, mono.get()));
  EXPECT_NEAR(0.0f, mono->channel(0)[0], 1e-6f);
  EXPECT_NEAR(100.0f / 32768.0f, mono->channel(0)[1], 1e-6f);
}

TEST(AudioBufferTest, ReadFramesIntoWiderBusSilencesExtraChannels) {
  // A mono source rendered into a stereo bus must not leave the previous
  // buffer's audio in channel 1: stale samples are audible as a stuck tone.
  std::vector<uint8_t> data(4 * 2, 0);
  for (int i = 0; i < 4; ++i) {
    const int16_t v = static_cast<int16_t>(1000 + i);
    std::memcpy(data.data() + i * 2, &v, 2);
  }
  auto buffer = AudioBuffer::Create(SampleFormat::kS16, ChannelLayout::kMono, 1,
                                    48000, 4, base::TimeDelta(),
                                    base::Microseconds(83), 0, std::move(data));
  auto bus = AudioBus::Create(2, 4);
  // Poison channel 1 first, then confirm ReadFrames clears it.
  for (int i = 0; i < 4; ++i) {
    bus->channel(1)[i] = 12345.0f;
  }
  EXPECT_EQ(4, buffer->ReadFrames(4, 0, bus.get()));
  EXPECT_NEAR(1000.0f / 32768.0f, bus->channel(0)[0], 1e-6f);
  for (int i = 0; i < 4; ++i) {
    EXPECT_FLOAT_EQ(0.0f, bus->channel(1)[i]) << "frame " << i;
  }
}

TEST(AudioBufferTest, EndOfStreamBufferReadsNothing) {
  auto eos = AudioBuffer::CreateEOSBuffer();
  ASSERT_TRUE(eos->end_of_stream());
  EXPECT_EQ(0, eos->frame_count());
  EXPECT_TRUE(eos->data().empty());
  auto bus = AudioBus::Create(2, 64);
  EXPECT_EQ(0, eos->ReadFrames(64, 0, bus.get()));
  EXPECT_NE(std::string::npos, eos->AsDebugString().find("EOS"));
}

TEST(AudioBufferTest, SerialIsMutableForSeekFlush) {
  auto buffer = MakeInterleavedS16(/*frames=*/2);
  EXPECT_EQ(7, buffer->serial());
  buffer->set_serial(42);
  EXPECT_EQ(42, buffer->serial());
}

TEST(AudioBufferTest, UnknownSampleFormatReadsNothingInsteadOfTrapping) {
  std::vector<uint8_t> data(32, 0xAB);
  auto buffer = AudioBuffer::Create(
      SampleFormat::kUnknown, ChannelLayout::kStereo, 2, 48000, 4,
      base::TimeDelta(), base::TimeDelta(), 0, std::move(data));
  auto bus = AudioBus::Create(2, 4);
  EXPECT_EQ(0, buffer->ReadFrames(4, 0, bus.get()));
}

TEST(AudioBufferTest, DecodeSampleCoversEveryFormat) {
  // Each format must map its full-scale value to ~1.0 (or -1.0 for the signed
  // minimum), otherwise the whole format is scaled wrong.
  const uint8_t u8_full = 255;
  EXPECT_NEAR(127.0f / 128.0f, DecodeSample(&u8_full, SampleFormat::kU8),
              1e-3f);

  const int16_t s16_min = -32768;
  EXPECT_NEAR(-1.0f,
              DecodeSample(reinterpret_cast<const uint8_t*>(&s16_min),
                           SampleFormat::kS16),
              1e-6f);

  const int32_t s32_max = 2147483647;
  EXPECT_NEAR(1.0f,
              DecodeSample(reinterpret_cast<const uint8_t*>(&s32_max),
                           SampleFormat::kS32),
              1e-6f);

  const float f32 = 0.5f;
  EXPECT_FLOAT_EQ(0.5f, DecodeSample(reinterpret_cast<const uint8_t*>(&f32),
                                     SampleFormat::kF32));
  // A null pointer must not dereference.
  EXPECT_FLOAT_EQ(0.0f, DecodeSample(nullptr, SampleFormat::kS16));
  EXPECT_FLOAT_EQ(0.0f, DecodeSample(nullptr, SampleFormat::kUnknown));
}

}  // namespace
}  // namespace avbase::media
