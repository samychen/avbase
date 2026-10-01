// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The frame-number encoding and the two decoders that produce it. The encoding
// is what every later assertion about "which frame is on screen" rests on, so
// it
// is tested on its own rather than only through a pipeline: a painter and a
// reader that disagree would make a seek test fail for the wrong reason.

#include "tests/support/synthetic_decoders.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/audio_buffer.h"
#include "media/base/decoder_buffer.h"
#include "media/base/decoder_status.h"
#include "media/base/demuxer_stream.h"
#include "media/base/video_frame.h"
#include "tests/support/synthetic_demuxer.h"

namespace avbase::media {
namespace {

using test::ReadFrameIndex;
using test::SyntheticAudioDecoder;
using test::SyntheticDemuxer;
using test::SyntheticSpec;
using test::SyntheticVideoDecoder;

base::scoped_refptr<VideoFrame> MakeFrame() {
  return VideoFrame::CreateBlackFrame(VideoFormat::kI420, Size{320, 240},
                                      Size{320, 240}, Rational{1, 1},
                                      base::TimeDelta(), base::Milliseconds(33),
                                      0);
}

// Pulls packets from |stream| until the EOS marker, then stops.
std::vector<base::scoped_refptr<DecoderBuffer>> ReadPackets(
    DemuxerStream* stream, size_t count) {
  std::vector<base::scoped_refptr<DecoderBuffer>> out;
  while (out.size() < count) {
    DemuxerStream::DecoderBufferVector batch;
    stream->Read(
        static_cast<uint32_t>(count),
        base::BindOnce(
            [](DemuxerStream::DecoderBufferVector* into,
               DemuxerStream::Status /*status*/,
               DemuxerStream::DecoderBufferVector buffers) {
              *into = std::move(buffers);
            },
            &batch));
    for (auto& buffer : batch) {
      if (buffer->IsEndOfStream()) {
        return out;
      }
      out.push_back(std::move(buffer));
    }
    if (batch.empty()) {
      return out;
    }
  }
  return out;
}

// A 32-bit value survives the round trip, including the extremes.
TEST(SyntheticDecodersTest, PaintsAndReadsBackTheIndex) {
  for (uint32_t index : {0u, 1u, 149u, 150u, 0x00FFFFFFu, 0xDEADBEEFu}) {
    auto frame = MakeFrame();
    test::PaintFrameIndex(frame.get(), index);
    uint32_t read_back = 0;
    ASSERT_TRUE(ReadFrameIndex(*frame, &read_back));
    EXPECT_EQ(read_back, index);
  }
}

// An unpainted frame reads as 0, which is the encoding's one ambiguity and the
// reason the decoder must paint every frame it emits: "no paint" and "frame 0"
// are the same picture.
TEST(SyntheticDecodersTest, AnUnpaintedFrameReadsAsZero) {
  auto frame = MakeFrame();
  uint32_t index = 12345;
  ASSERT_TRUE(ReadFrameIndex(*frame, &index));
  EXPECT_EQ(index, 0u);
}

// The video decoder turns the packet's index into pixels and carries the
// packet's timestamp through, which is what lets a seek assertion compare
// against the same numbers the demuxer's tests use.
TEST(SyntheticDecodersTest, VideoDecoderPaintsThePacketsIndex) {
  SyntheticSpec spec;
  SyntheticDemuxer demuxer(spec);
  demuxer.Initialize(DataSourceDescriptor(), DemuxerOptions(), nullptr, nullptr,
                     base::BindOnce([](Status) {}));
  DemuxerStream* stream = demuxer.GetStream(DemuxerStreamType::kVideo);
  ASSERT_NE(stream, nullptr);
  const auto packets = ReadPackets(stream, 3);
  ASSERT_EQ(packets.size(), 3u);

  demuxer.SetPosition(base::TimeDelta());
  SyntheticVideoDecoder decoder(spec);
  std::vector<base::scoped_refptr<VideoFrame>> frames;
  decoder.Initialize(
      stream->video_decoder_config(), false, nullptr,
      base::BindOnce([](DecoderStatus) {}),
      base::BindRepeating(
          [](std::vector<base::scoped_refptr<VideoFrame>>* into,
             base::scoped_refptr<VideoFrame> frame) { into->push_back(frame); },
          &frames),
      base::BindRepeating([](WaitingReason) {}));

  for (const auto& packet : packets) {
    decoder.Decode(packet, base::BindOnce([](DecoderStatus) {}));
  }
  ASSERT_EQ(frames.size(), 3u);
  for (size_t i = 0; i < frames.size(); ++i) {
    uint32_t index = 0;
    ASSERT_TRUE(ReadFrameIndex(*frames[i], &index));
    EXPECT_EQ(index, i);
    EXPECT_EQ(frames[i]->timestamp(), packets[i]->timestamp());
  }
  EXPECT_EQ(decoder.frames_output(), 3);
}

// The audio half: the tone of a packet is 440 Hz plus the whole second of its
// timestamp, and the samples carry it. Counted as zero crossings rather than
// with an FFT -- an FFT is what the later checklist items need, and a count is
// enough to catch a decoder that ignored the timestamp and played 440 Hz
// everywhere.
TEST(SyntheticDecodersTest, AudioDecoderProducesThePacketsTone) {
  SyntheticSpec spec;
  SyntheticDemuxer demuxer(spec);
  demuxer.Initialize(DataSourceDescriptor(), DemuxerOptions(), nullptr, nullptr,
                     base::BindOnce([](Status) {}));
  DemuxerStream* stream = demuxer.GetStream(DemuxerStreamType::kAudio);
  ASSERT_NE(stream, nullptr);
  // Skip into the second second, where the tone is 441 Hz.
  const auto packets = ReadPackets(stream, 50);
  ASSERT_GT(packets.size(), 48u);
  const auto packet = packets[47];
  EXPECT_GE(packet->timestamp(), base::Seconds(1));

  SyntheticAudioDecoder decoder(spec);
  std::vector<base::scoped_refptr<AudioBuffer>> buffers;
  decoder.Initialize(
      stream->audio_decoder_config(), false, packet->serial(),
      base::BindOnce([](DecoderStatus) {}),
      base::BindRepeating(
          [](std::vector<base::scoped_refptr<AudioBuffer>>* into,
             base::scoped_refptr<AudioBuffer> buffer) {
            into->push_back(buffer);
          },
          &buffers),
      base::BindRepeating([](WaitingReason) {}));
  decoder.Decode(packet, base::BindOnce([](DecoderStatus) {}));

  ASSERT_EQ(buffers.size(), 1u);
  const auto& buffer = buffers[0];
  EXPECT_EQ(buffer->timestamp(), packet->timestamp());
  EXPECT_EQ(buffer->frame_count(), spec.audio_frames_per_packet);

  const std::span<const uint8_t> raw = buffer->channel_data(0);
  const auto* samples = reinterpret_cast<const float*>(raw.data());
  int crossings = 0;
  for (int i = 1; i < buffer->frame_count(); ++i) {
    if ((samples[i - 1] < 0.0f) != (samples[i] < 0.0f)) {
      ++crossings;
    }
  }
  // 441 Hz over 1024 frames at 48 kHz is 9.4 cycles, so about 19 crossings.
  EXPECT_GE(crossings, 18);
  EXPECT_LE(crossings, 20);
}

}  // namespace
}  // namespace avbase::media
