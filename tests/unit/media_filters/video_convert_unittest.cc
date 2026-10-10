// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/video_convert.h"

#include <cstdlib>

#include <gtest/gtest.h>

#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace avbase::media::ffmpeg {
namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 48;

// Builds a planar frame filled with a deterministic gradient (no two planes
// identical, no flat padding), owned via FramePtr.
FramePtr MakePlanarFrame(AVPixelFormat format, int colorspace,
                         int color_range) {
  FramePtr frame(av_frame_alloc());
  frame->width = kWidth;
  frame->height = kHeight;
  frame->format = format;
  frame->colorspace = static_cast<AVColorSpace>(colorspace);
  frame->color_range = static_cast<AVColorRange>(color_range);
  EXPECT_EQ(av_frame_get_buffer(frame.get(), 32), 0);
  // Chroma planes are vertically subsampled: NV12's UV plane and I420's U/V
  // planes are allocated half the frame's height. Walking every plane for
  // kHeight rows used to write past the end of their buffer, which nothing
  // noticed until ASan aborted the three tests that build a subsampled
  // source (the one test built on YUV422P, whose planes are all full height,
  // always passed — that correlation is what identified this).
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
  const int chroma_shift = desc ? desc->log2_chroma_h : 0;
  int seed = 0;
  for (int p = 0; p < 4; ++p) {
    if (!frame->data[p]) {
      continue;
    }
    const int rows = p == 0 ? kHeight : AV_CEIL_RSHIFT(kHeight, chroma_shift);
    for (int y = 0; y < rows; ++y) {
      for (int x = 0; x < frame->linesize[p]; ++x) {
        frame->data[p][y * frame->linesize[p] + x] =
            static_cast<uint8_t>((seed++ * 37 + 11) & 0xff);
      }
    }
  }
  return frame;
}

// Direct sws reference. Pass |set_details| for YUV→RGB conversions where the
// colorspace details carry the matrix; leave it off for pure YUV→YUV layout
// changes, where setting details would route sws through an 8-bit RGB
// intermediate and requantize Y (see VideoConverter::ConvertSws).
FramePtr ConvertWithSws(AVFrame& src, AVPixelFormat dst_format,
                        int coefficients, int src_range, bool set_details) {
  SwsPtr sws(sws_getContext(
      src.width, src.height, static_cast<AVPixelFormat>(src.format), kWidth,
      kHeight, dst_format, SWS_BILINEAR, nullptr, nullptr, nullptr));
  EXPECT_TRUE(sws);
  if (set_details) {
    sws_setColorspaceDetails(sws.get(), sws_getCoefficients(coefficients),
                             src_range, sws_getCoefficients(SWS_CS_DEFAULT), 0,
                             0, 1 << 16, 1 << 16);
  }
  FramePtr dst(av_frame_alloc());
  dst->width = kWidth;
  dst->height = kHeight;
  dst->format = dst_format;
  EXPECT_EQ(av_frame_get_buffer(dst.get(), 32), 0);
  EXPECT_EQ(sws_scale(sws.get(), src.data, src.linesize, 0, src.height,
                      dst->data, dst->linesize),
            src.height);
  return dst;
}

FramePtr MakeDstFrame(AVPixelFormat format) {
  FramePtr dst(av_frame_alloc());
  dst->width = kWidth;
  dst->height = kHeight;
  dst->format = format;
  EXPECT_EQ(av_frame_get_buffer(dst.get(), 32), 0);
  return dst;
}

// Total absolute byte difference across a plane.
int64_t PlaneDiff(const uint8_t* a, const uint8_t* b, int bytes) {
  int64_t diff = 0;
  for (int i = 0; i < bytes; ++i) {
    diff += std::abs(a[i] - b[i]);
  }
  return diff;
}

TEST(VideoConverterTest, ConvertWithoutConfigureFails) {
  VideoConverter converter;
  ASSERT_FALSE(converter.Configured());
  FramePtr src =
      MakePlanarFrame(AV_PIX_FMT_YUV420P, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG);
  uint8_t sink[kWidth * kHeight] = {0};
  uint8_t* dst_data[4] = {sink, nullptr, nullptr, nullptr};
  int dst_linesize[4] = {kWidth, 0, 0, 0};
  EXPECT_FALSE(converter.Convert(*src, dst_data, dst_linesize));
}

TEST(VideoConverterTest, Nv12ToI420MatchesPlaneSplit) {
  VideoConverter converter;
  ASSERT_TRUE(converter.Configure(kWidth, kHeight, AV_PIX_FMT_NV12, kWidth,
                                  kHeight, AV_PIX_FMT_YUV420P));
  FramePtr src =
      MakePlanarFrame(AV_PIX_FMT_NV12, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG);
  FramePtr dst = MakeDstFrame(AV_PIX_FMT_YUV420P);
  uint8_t* dst_data[4] = {dst->data[0], dst->data[1], dst->data[2],
                          dst->data[3]};
  int dst_linesize[4] = {dst->linesize[0], dst->linesize[1], dst->linesize[2],
                         dst->linesize[3]};
  EXPECT_TRUE(converter.Convert(*src, dst_data, dst_linesize));

  // Pure layout conversion: Y identical; U/V de-interleave NV12's chroma
  // (U lives at even offsets of the interleaved plane, V at odd ones).
  EXPECT_EQ(PlaneDiff(src->data[0], dst->data[0],
                      kHeight * std::min(src->linesize[0], dst->linesize[0])),
            0);
  for (int p = 1; p <= 2; ++p) {
    const int offset = p - 1;
    for (int y = 0; y < kHeight / 2; ++y) {
      for (int x = 0; x < kWidth / 2; ++x) {
        ASSERT_EQ(src->data[1][y * src->linesize[1] + 2 * x + offset],
                  dst->data[p][y * dst->linesize[p] + x]);
      }
    }
  }
}

TEST(VideoConverterTest, Yuv422ToI420MatchesSws) {
  VideoConverter converter;
  ASSERT_TRUE(converter.Configure(kWidth, kHeight, AV_PIX_FMT_YUV422P, kWidth,
                                  kHeight, AV_PIX_FMT_YUV420P));
  FramePtr src =
      MakePlanarFrame(AV_PIX_FMT_YUV422P, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG);
  FramePtr reference =
      ConvertWithSws(*src, AV_PIX_FMT_YUV420P, SWS_CS_ITU709, 0, false);
  FramePtr dst = MakeDstFrame(AV_PIX_FMT_YUV420P);
  uint8_t* dst_data[4] = {dst->data[0], dst->data[1], dst->data[2],
                          dst->data[3]};
  int dst_linesize[4] = {dst->linesize[0], dst->linesize[1], dst->linesize[2],
                         dst->linesize[3]};
  EXPECT_TRUE(converter.Convert(*src, dst_data, dst_linesize));

  // Both subsample the same chroma; kernels may round differently, so allow
  // a small per-plane deviation rather than exact equality.
  EXPECT_LE(
      PlaneDiff(reference->data[0], dst->data[0],
                kHeight * std::min(reference->linesize[0], dst->linesize[0])),
      static_cast<int64_t>(kWidth) * kHeight);
}

TEST(VideoConverterTest, I420ToBgraHonorsColorspace) {
  // BT.709 source through the converter vs sws configured for 709: the libyuv
  // fast path must carry the frame's matrix, not default to BT.601.
  VideoConverter converter;
  ASSERT_TRUE(converter.Configure(kWidth, kHeight, AV_PIX_FMT_YUV420P, kWidth,
                                  kHeight, AV_PIX_FMT_0RGB32));
  FramePtr src =
      MakePlanarFrame(AV_PIX_FMT_YUV420P, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG);
  FramePtr reference =
      ConvertWithSws(*src, AV_PIX_FMT_0RGB32, SWS_CS_ITU709, 0, true);
  FramePtr dst = MakeDstFrame(AV_PIX_FMT_0RGB32);
  uint8_t* dst_data[4] = {dst->data[0], dst->data[1], dst->data[2],
                          dst->data[3]};
  int dst_linesize[4] = {dst->linesize[0], dst->linesize[1], dst->linesize[2],
                         dst->linesize[3]};
  EXPECT_TRUE(converter.Convert(*src, dst_data, dst_linesize));

  const int64_t diff =
      PlaneDiff(reference->data[0], dst->data[0],
                kHeight * std::min(reference->linesize[0], dst->linesize[0]));
  // Tolerance calibrated against this exact fixture, per BYTE of RGBA output:
  // a correct-matrix libyuv vs sws-709 differs ~1.3 bytes/byte (fixed-point
  // rounding), while a wrong matrix (601 vs 709) differs ~6.4. 3 separates
  // the two with headroom on both sides.
  EXPECT_LE(diff, static_cast<int64_t>(kWidth) * kHeight * 4 * 3);
}

TEST(VideoConverterTest, ConfigureChangeRebuildsFallback) {
  VideoConverter converter;
  ASSERT_TRUE(converter.Configure(kWidth, kHeight, AV_PIX_FMT_YUV422P, kWidth,
                                  kHeight, AV_PIX_FMT_YUV420P));
  EXPECT_TRUE(converter.Configured());
  // Same geometry: still configured. A different pair must re-configure
  // successfully rather than reuse the old context.
  EXPECT_TRUE(converter.Configure(kWidth, kHeight, AV_PIX_FMT_NV12, kWidth,
                                  kHeight, AV_PIX_FMT_YUV420P));
  EXPECT_TRUE(converter.Configured());
  converter.Reset();
  EXPECT_FALSE(converter.Configured());
}

}  // namespace
}  // namespace avbase::media::ffmpeg
