// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The synthetic source's own tests, which are what makes it usable as an oracle
// elsewhere: a test double whose cadence, keyframe geometry and seek landing
// are
// unspecified is a second thing that can be wrong, and when it is, the failure
// looks like a pipeline bug.
//
// Everything asserted here is a claim docs/07 §5 makes about the source, stated
// as "read the packet back and check": the frame index travels in the payload,
// the geometry in the timestamp, and a keyframe seek lands at or before the
// request. The pixel half of the encoding (the frame number painted into the
// frame's corner) is not asserted because the decoder that paints it does not
// exist yet; that arrives with the pipeline-level seek test it is for.

#include "tests/support/synthetic_demuxer.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/decoder_buffer.h"
#include "media/base/demuxer_stream.h"

namespace avbase::media {
namespace {

using test::ExpectedToneHzAt;
using test::ReadIndexPayload;
using test::SyntheticDemuxer;
using test::SyntheticSpec;

class SyntheticDemuxerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    demuxer_ = std::make_unique<SyntheticDemuxer>(SyntheticSpec());
    bool initialized = false;
    demuxer_->Initialize(
        DataSourceDescriptor(), DemuxerOptions(), /*host=*/nullptr,
        env_.GetMainThreadTaskRunnerRef(),
        base::BindOnce(
            [](bool* done, Status status) { *done = status.has_value(); },
            &initialized));
    env_.RunUntilIdle();
    ASSERT_TRUE(initialized);
  }

  // One Read(). The synthetic streams complete inline (they own no I/O), which
  // is what the demuxer's contract allows for a source that never blocks.
  DemuxerStream::DecoderBufferVector ReadOnce(DemuxerStream* stream,
                                             uint32_t count = 16) {
    DemuxerStream::DecoderBufferVector result;
    stream->Read(count,
                 base::BindOnce(
                     [](DemuxerStream::DecoderBufferVector* out,
                        DemuxerStream::Status /*status*/,
                        DemuxerStream::DecoderBufferVector buffers) {
                       *out = std::move(buffers);
                     },
                     &result));
    return result;
  }

  // Everything the stream has, up to a bound, stopping at the EOS marker.
  DemuxerStream::DecoderBufferVector Drain(DemuxerStream* stream) {
    DemuxerStream::DecoderBufferVector all;
    while (all.size() < 10000) {
      DemuxerStream::DecoderBufferVector batch = ReadOnce(stream);
      if (batch.empty()) {
        break;
      }
      if (batch.back()->IsEndOfStream()) {
        batch.pop_back();
        all.insert(all.end(), batch.begin(), batch.end());
        break;
      }
      all.insert(all.end(), batch.begin(), batch.end());
    }
    return all;
  }

  uint32_t IndexOf(const base::scoped_refptr<DecoderBuffer>& buffer) {
    uint32_t index = 0;
    EXPECT_TRUE(ReadIndexPayload(*buffer, &index));
    return index;
  }

  base::test::TaskEnvironment env_;
  std::unique_ptr<SyntheticDemuxer> demuxer_;
};

// The cadence and the encoding together: 10 s at 30 fps is 300 packets, packet
// n
// carries index n, and its timestamp is n/30 s exactly. Exactness is the point
// --
// 1/30 s is not representable in microseconds, so this catches a source that
// accumulates its clock instead of computing it.
TEST_F(SyntheticDemuxerTest, VideoPacketsCarryTheirIndexAndCadence) {
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_NE(video, nullptr);
  const DemuxerStream::DecoderBufferVector packets = Drain(video);

  ASSERT_EQ(packets.size(), 300u);
  for (size_t i = 0; i < packets.size(); ++i) {
    EXPECT_EQ(IndexOf(packets[i]), i);
    // Computed from the index, not by multiplying one frame's duration by it:
    // 1/30 s is 33333 us truncated, and multiplying that by 3 gives 99999, not
    // the 100000 us the source reports. That difference is the property this
    // line exists to check -- the timestamp must not accumulate error.
    const base::TimeDelta expected_pts =
        base::Microseconds(1000000LL * static_cast<int64_t>(i) / 30);
    EXPECT_EQ(packets[i]->timestamp(), expected_pts) << "frame " << i;
    const bool expected_keyframe = (i % 30) == 0;
    EXPECT_EQ(packets[i]->is_keyframe(), expected_keyframe) << "frame " << i;
  }
  // The frame the seek test lands on, stated once here: frame 150 is 5 s.
  EXPECT_EQ(packets[150]->timestamp(), base::Seconds(5));
}

// The claim a seek test rests on: a physical seek stops at a keyframe at or
// before the request, and reports where it actually landed. The *accurate*
// landing (5.017 s -> frame 151, which docs/07 §5's checklist lists) is not the
// demuxer's: it is the pipeline seeking precisely after the keyframe, which is
// M9's SeekController and an existing open item.
TEST_F(SyntheticDemuxerTest, SeekLandsOnTheKeyframeAtOrBeforeTheRequest) {
  EXPECT_EQ(demuxer_->KeyframeAtOrBefore(base::Seconds(5)), base::Seconds(5));
  EXPECT_EQ(demuxer_->KeyframeAtOrBefore(base::Milliseconds(5500)),
            base::Seconds(5));
  EXPECT_EQ(demuxer_->KeyframeAtOrBefore(base::Milliseconds(17)),
            base::TimeDelta());

  base::TimeDelta actual;
  demuxer_->StartPlayingFrom(
      base::Seconds(5), base::BindOnce(
                            [](base::TimeDelta* out, Status status,
                               base::TimeDelta landed) {
                              EXPECT_TRUE(status.has_value());
                              *out = landed;
                            },
                            &actual));
  EXPECT_EQ(actual, base::Seconds(5));
  EXPECT_EQ(demuxer_->seek_count(), 1);

  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  const DemuxerStream::DecoderBufferVector after_seek = ReadOnce(video, 1);
  ASSERT_EQ(after_seek.size(), 1u);
  EXPECT_EQ(IndexOf(after_seek[0]), 150);

  // And the mid-keyframe case: 5.5 s still lands on 5 s, with the keyframe flag
  // set, so a decoder can start there.
  demuxer_->StartPlayingFrom(base::Milliseconds(5500),
                             base::BindOnce([](Status, base::TimeDelta) {}));
  const DemuxerStream::DecoderBufferVector mid = ReadOnce(video, 1);
  ASSERT_EQ(mid.size(), 1u);
  EXPECT_EQ(IndexOf(mid[0]), 150);
  EXPECT_TRUE(mid[0]->is_keyframe());
  EXPECT_EQ(mid[0]->timestamp(), base::Seconds(5));
}

// The audio half of the encoding: a packet's tone is its whole second plus 440,
// so the timeline is readable from the samples. The boundary is what matters --
// the packet before 1 s is 440 Hz, the packet after it is 441 Hz -- and that is
// asserted against the packet's own timestamp, not against a counter.
TEST_F(SyntheticDemuxerTest, AudioPacketsCarryTheToneOfTheirSecond) {
  DemuxerStream* audio = demuxer_->GetStream(DemuxerStreamType::kAudio);
  ASSERT_NE(audio, nullptr);
  const DemuxerStream::DecoderBufferVector packets = ReadOnce(audio, 200);
  ASSERT_GE(packets.size(), 100u);

  for (size_t i = 0; i < packets.size(); ++i) {
    EXPECT_EQ(IndexOf(packets[i]), i);
    const base::TimeDelta pts = packets[i]->timestamp();
    // The whole seconds of the packet's own timestamp, computed here rather
    // than
    // through the helper the source uses.
    const double expected =
        440.0 + static_cast<double>(pts.InMilliseconds() / 1000);
    EXPECT_DOUBLE_EQ(ExpectedToneHzAt(pts, 440.0), expected)
        << "at " << pts.InMilliseconds() << " ms";
    EXPECT_TRUE(packets[i]->is_keyframe());
  }
  // The boundary, which is the part worth naming: 1024 frames at 48 kHz means
  // the 47th packet is the first one at or past 1 s, and 94/141/188 are the
  // first at or past 2/3/4 s. A source that stamped seconds from a counter
  // instead of from the timestamp would agree with itself here and disagree at
  // 5 s, after 234 packets of accumulated rounding.
  EXPECT_LT(packets[46]->timestamp(), base::Seconds(1));
  EXPECT_GE(packets[47]->timestamp(), base::Seconds(1));
  EXPECT_DOUBLE_EQ(ExpectedToneHzAt(packets[46]->timestamp(), 440.0), 440.0);
  EXPECT_DOUBLE_EQ(ExpectedToneHzAt(packets[47]->timestamp(), 440.0), 441.0);
  EXPECT_DOUBLE_EQ(ExpectedToneHzAt(packets[94]->timestamp(), 440.0), 442.0);
  EXPECT_DOUBLE_EQ(ExpectedToneHzAt(packets[188]->timestamp(), 440.0), 444.0);
}

// Reading past the end is not an error and not a hang: it is one EOS marker,
// after which the stream keeps answering with EOS.
TEST_F(SyntheticDemuxerTest, EndOfStreamIsReportedAfterTheLastPacket) {
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  demuxer_->SetPosition(demuxer_->spec().duration - base::Milliseconds(1));

  const DemuxerStream::DecoderBufferVector tail = Drain(video);
  ASSERT_EQ(tail.size(), 1u);   // frame 299 is the last one inside 10 s
  EXPECT_EQ(IndexOf(tail[0]), 299);

  const DemuxerStream::DecoderBufferVector again = ReadOnce(video, 1);
  ASSERT_EQ(again.size(), 1u);
  EXPECT_TRUE(again[0]->IsEndOfStream());
}

// Flush and Reset rewind both streams together. A double that let one of them
// keep its cursor would make every later assertion about the other one wrong.
TEST_F(SyntheticDemuxerTest, FlushRewindsBothStreams) {
  demuxer_->SetPosition(base::Seconds(9));
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  DemuxerStream* audio = demuxer_->GetStream(DemuxerStreamType::kAudio);
  const DemuxerStream::DecoderBufferVector before_video = ReadOnce(video, 1);
  const DemuxerStream::DecoderBufferVector before_audio = ReadOnce(audio, 1);
  ASSERT_EQ(before_video.size(), 1u);
  ASSERT_EQ(before_audio.size(), 1u);
  EXPECT_EQ(IndexOf(before_video[0]), 270);
  EXPECT_GT(IndexOf(before_audio[0]), 400u);

  bool flushed = false;
  demuxer_->Flush(base::BindOnce([](bool* done) { *done = true; }, &flushed));
  EXPECT_TRUE(flushed);
  EXPECT_EQ(demuxer_->position(), base::TimeDelta());
  const DemuxerStream::DecoderBufferVector after_video = ReadOnce(video, 1);
  const DemuxerStream::DecoderBufferVector after_audio = ReadOnce(audio, 1);
  ASSERT_EQ(after_video.size(), 1u);
  ASSERT_EQ(after_audio.size(), 1u);
  EXPECT_EQ(IndexOf(after_video[0]), 0);
  EXPECT_EQ(IndexOf(after_audio[0]), 0u);
}

// The container-shaped facts the pipeline reads before playing: duration,
// stream list, seekability, and the configs the renderers are handed.
TEST_F(SyntheticDemuxerTest, MediaInfoAndConfigsMatchTheSpec) {
  const MediaInfo& info = demuxer_->media_info();
  EXPECT_EQ(info.duration, base::Seconds(10));
  EXPECT_TRUE(info.seekable);
  EXPECT_EQ(info.streams.size(), 2u);
  EXPECT_EQ(info.FirstStreamOfKind(StreamKind::kVideo), 0);
  EXPECT_EQ(info.FirstStreamOfKind(StreamKind::kAudio), 1);
  EXPECT_FALSE(demuxer_->IsLive());
  EXPECT_TRUE(demuxer_->IsSeekable());
  EXPECT_EQ(demuxer_->GetStartTime(), base::TimeDelta());

  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  EXPECT_EQ(video->video_decoder_config().coded_size, (Size{320, 240}));
  // Compared field by field: Rational has no operator==, and giving it one for
  // a test would be a change to a frozen media/base header.
  EXPECT_EQ(video->video_decoder_config().frame_rate.num, 30);
  EXPECT_EQ(video->video_decoder_config().frame_rate.den, 1);
  EXPECT_TRUE(video->video_decoder_config().IsValidConfig());
  EXPECT_EQ(video->type(), DemuxerStreamType::kVideo);
  EXPECT_EQ(video->stream_index(), 0);

  DemuxerStream* audio = demuxer_->GetStream(DemuxerStreamType::kAudio);
  EXPECT_TRUE(audio->audio_decoder_config().IsValidConfig());
  EXPECT_EQ(audio->audio_decoder_config().sample_rate, 48000);
  EXPECT_EQ(audio->type(), DemuxerStreamType::kAudio);
  EXPECT_EQ(demuxer_->GetStream(DemuxerStreamType::kText), nullptr);
}

}  // namespace
}  // namespace avbase::media
