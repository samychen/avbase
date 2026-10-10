// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// H7: the transcode video timeline. The old encoder set time_base from the
// fps DENOMINATOR ({1,1} for every integer-rate source) and fed raw
// microseconds as pts, so a 30 fps source came out with a timeline scaled by
// ~10^6; the frame converter hardcoded 1/90000 for every container. Both are
// asserted here against values no wrong unit can satisfy.

#include "media/transcode/ffmpeg_transcode_streams.h"
#include "media/transcode/ffmpeg_video_encoder.h"

#include <cstring>
#include <vector>

#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/encoded_packet.h"
#include "media/base/video_frame.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace avbase::media {
namespace ff = ffmpeg;
namespace {

// One frame interval of a 30 fps source, in microseconds.
constexpr int64_t kFrameIntervalUs = 33333;

base::scoped_refptr<VideoFrame> MakeGrayFrame(base::TimeDelta ts) {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{16, 16}, Size{16, 16}, Rational{30, 1}, ts,
      base::Microseconds(kFrameIntervalUs), 0);
  return frame;
}

TEST(VideoEncoderTimeBaseTest, TimeBaseIsTheInverseFrameRate) {
  FFmpegVideoEncoder encoder;
  FFmpegVideoEncoder::Params params;
  params.codec_name = "mjpeg";
  params.width = 16;
  params.height = 16;
  params.fps_num = 30;
  params.fps_den = 1;
  ASSERT_TRUE(encoder.Initialize(params));
  const AVRational tb = encoder.time_base();
  EXPECT_EQ(tb.num, 1);
  EXPECT_EQ(tb.den, 30)
      << "time_base must be the inverse frame rate, not {1, fps_den}";
}

// Uniform 30 fps input must come out as one tick per frame in the encoder's
// {1,30} time_base: pts 0, 1, 2, ... The old unit mismatch produced
// pts 0, 33333, 66666, ... -- a timeline where every frame lasted ~9 hours.
TEST(VideoEncoderTimeBaseTest, UniformFpsInputProducesConsecutivePts) {
  FFmpegVideoEncoder encoder;
  FFmpegVideoEncoder::Params params;
  params.codec_name = "mjpeg";
  params.width = 16;
  params.height = 16;
  params.fps_num = 30;
  params.fps_den = 1;
  ASSERT_TRUE(encoder.Initialize(params));

  std::vector<EncodedPacket> out;
  for (int i = 0; i < 8; ++i) {
    std::vector<EncodedPacket> packets;
    ASSERT_TRUE(encoder.Encode(MakeGrayFrame(base::Microseconds(
                                   static_cast<int64_t>(i) *
                                   kFrameIntervalUs)),
                               &packets));
    out.insert(out.end(), std::make_move_iterator(packets.begin()),
               std::make_move_iterator(packets.end()));
  }
  std::vector<EncodedPacket> flushed;
  ASSERT_TRUE(encoder.Flush(&flushed));
  out.insert(out.end(), std::make_move_iterator(flushed.begin()),
             std::make_move_iterator(flushed.end()));

  // mjpeg has no B-frames and no encoder-side delay: every input frame yields
  // exactly one packet immediately, with pts == dts.
  ASSERT_EQ(out.size(), 8u);
  for (size_t i = 0; i < out.size(); ++i) {
    EXPECT_EQ(out[i].pts, static_cast<int64_t>(i))
        << "packet " << i << " pts is not one tick per frame interval";
    EXPECT_EQ(out[i].dts, out[i].pts);
    if (i > 0) {
      EXPECT_EQ(out[i].pts - out[i - 1].pts, 1);
    }
  }
}

// A non-integer rate exercises the rescale instead of the degenerate {1,1}:
// 29.97 fps (30000/1001) -> one input frame interval maps to 1001/30000 s,
// so consecutive frames differ by 1001 ticks of a {1001, 30000} time_base.
TEST(VideoEncoderTimeBaseTest, NonIntegerRateStaysOnTheTimeline) {
  FFmpegVideoEncoder encoder;
  FFmpegVideoEncoder::Params params;
  params.codec_name = "mjpeg";
  params.width = 16;
  params.height = 16;
  params.fps_num = 30000;
  params.fps_den = 1001;
  ASSERT_TRUE(encoder.Initialize(params));
  const AVRational tb = encoder.time_base();
  ASSERT_EQ(tb.num, 1001);
  ASSERT_EQ(tb.den, 30000);

  std::vector<EncodedPacket> out;
  for (int i = 0; i < 4; ++i) {
    std::vector<EncodedPacket> packets;
    // Microsecond timestamps of the ideal 30000/1001 grid.
    const int64_t us = static_cast<int64_t>(i) * 1001 * 1000000 / 30000;
    ASSERT_TRUE(encoder.Encode(MakeGrayFrame(base::Microseconds(us)),
                               &packets));
    out.insert(out.end(), std::make_move_iterator(packets.begin()),
               std::make_move_iterator(packets.end()));
  }
  ASSERT_EQ(out.size(), 4u);
  for (size_t i = 0; i < out.size(); ++i) {
    // With time_base {1001, 30000} one tick IS one frame interval
    // (1001/30000 s), so the ideal grid lands on tick i for any fps.
    EXPECT_EQ(out[i].pts, static_cast<int64_t>(i))
        << "packet " << i << " drifted off the frame grid";
  }
}

// The decode-side converter must honour the pkt_timebase it is handed: a
// container with time_base 1/1000 and pts 5000 means 5 s, not the 55.6 ms
// the hardcoded /90000 produced.
TEST(AvFrameToVideoFrameTest, HonoursTheGivenTimeBase) {
  ff::FramePtr frame(av_frame_alloc());
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = 4;
  frame->height = 4;
  ASSERT_EQ(av_frame_get_buffer(frame.get(), 32), 0);
  memset(frame->data[0], 0x10, static_cast<size_t>(frame->linesize[0]) * 4);
  memset(frame->data[1], 0x80, static_cast<size_t>(frame->linesize[1]) * 2);
  memset(frame->data[2], 0x80, static_cast<size_t>(frame->linesize[2]) * 2);
  frame->pts = 5000;  // 5000 * (1/1000) s = 5 s.
  frame->duration = 33;

  auto vf = AvFrameToVideoFrame(frame.get(), 4, 4, AVRational{1, 1000});
  ASSERT_TRUE(vf);
  EXPECT_EQ(vf->timestamp(), base::Seconds(5));
  EXPECT_EQ(vf->duration(), base::Milliseconds(33));

  // The historical 1/90000 time_base still gives the same answer as before.
  frame->pts = 90000;
  auto vf2 = AvFrameToVideoFrame(frame.get(), 4, 4, AVRational{1, 90000});
  ASSERT_TRUE(vf2);
  EXPECT_EQ(vf2->timestamp(), base::Seconds(1));
}

}  // namespace
}  // namespace avbase::media
