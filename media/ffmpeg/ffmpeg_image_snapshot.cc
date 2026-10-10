// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_image_snapshot.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "base/logging.h"
#include "media/base/video_frame.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"
#include "media/ffmpeg/video_convert.h"

namespace avbase::media {
namespace {

namespace ff = ::avbase::media::ffmpeg;

// The JPEG encoder's native input is YUVJ420P (full-range); FFmpeg 6+ also
// accepts YUV420P but older builds do not, and the mjpeg encoder may simply
// be absent from a --disable-everything build. Every failure funnels into one
// actionable error rather than a bare ret code.
Status EncodeToJpeg(AVCodecContext* ctx, const AVFrame& frame,
                    std::vector<uint8_t>* out) {
  const int send = avcodec_send_frame(ctx, &frame);
  if (send != 0) {
    return base::unexpected(MediaError(
        ErrorCode::kDecodeFailed, "the snapshot encoder rejected the frame",
        "avcodec_send_frame: " + ff::AvErrorString(send),
        "this is an avbase bug; report the event log"));
  }
  ff::PacketPtr packet = ff::MakePacket();
  const int receive = avcodec_receive_packet(ctx, packet.get());
  if (receive != 0) {
    return base::unexpected(MediaError(
        ErrorCode::kDecodeFailed, "the snapshot encoder produced no packet",
        "avcodec_receive_packet: " + ff::AvErrorString(receive),
        "this is an avbase bug; report the event log"));
  }
  out->assign(packet->data, packet->data + packet->size);
  return Status();
}

}  // namespace

Status WriteJpegSnapshot(const VideoFrame& frame, const std::string& path) {
  const AVCodec* codec = avcodec_find_encoder_by_name("mjpeg");
  if (!codec) {
    return base::unexpected(MediaError(
        ErrorCode::kNotImplemented, "this build's FFmpeg has no mjpeg encoder",
        "avcodec_find_encoder_by_name(\"mjpeg\") returned null",
        "tools/setup_ffmpeg.sh enables it; re-run the script to refresh the "
        "prefix"));
  }
  ff::CodecCtxPtr ctx(avcodec_alloc_context3(codec));
  if (!ctx) {
    // M10: OOM used to null-deref on ctx->width below.
    return base::unexpected(MediaError(
        ErrorCode::kOutOfMemory, "cannot allocate the mjpeg encoder context",
        "avcodec_alloc_context3 returned null",
        "retry with a smaller frame or free memory"));
  }
  const int width = frame.coded_size().width;
  const int height = frame.coded_size().height;
  ctx->width = width;
  ctx->height = height;
  ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
  ctx->time_base = AVRational{1, 1};
  if (const int open = avcodec_open2(ctx.get(), codec, nullptr); open != 0) {
    return base::unexpected(MediaError(
        ErrorCode::kNotImplemented, "the mjpeg encoder could not be opened",
        "avcodec_open2: " + ff::AvErrorString(open),
        "this is an avbase bug; report the event log"));
  }

  // The frame may arrive in any avbase format and with any stride layout;
  // normalize to full-range YUVJ420P through the same converter the decode
  // path uses (unmarked YUV is treated as limited-range BT.601, matching
  // video_convert's default -- snapshots of HD content with an explicit
  // BT.709 tag keep their matrix).
  ff::VideoConverter converter;
  if (!converter.Configure(width, height,
                           ff::AvPixelFormatFromVideoFormat(frame.format()),
                           width, height, AV_PIX_FMT_YUV420P)) {
    return base::unexpected(
        MediaError(ErrorCode::kInvalidArgument,
                   "the frame's format cannot be converted for a snapshot",
                   "format " + std::to_string(static_cast<int>(frame.format())),
                   "this is an avbase bug; report the event log"));
  }
  ff::FramePtr src = ff::MakeFrame();
  src->width = width;
  src->height = height;
  src->format = ff::AvPixelFormatFromVideoFormat(frame.format());
  switch (frame.color_space().matrix) {
  case media::ColorMatrix::kBT709:
    src->colorspace = AVCOL_SPC_BT709;
    break;
  case media::ColorMatrix::kBT2020Ncl:
    src->colorspace = AVCOL_SPC_BT2020_NCL;
    break;
  case media::ColorMatrix::kBT2020Cl:
    src->colorspace = AVCOL_SPC_BT2020_CL;
    break;
  case media::ColorMatrix::kSMPTE170M:
    src->colorspace = AVCOL_SPC_SMPTE170M;
    break;
  default:
    src->colorspace = AVCOL_SPC_BT470BG;
    break;
  }
  src->color_range = frame.color_space().range == media::ColorRange::kFull
                         ? AVCOL_RANGE_JPEG
                         : AVCOL_RANGE_MPEG;
  for (int p = 0; p < VideoFrame::kMaxPlanes; ++p) {
    const auto plane = static_cast<VideoFrame::Plane>(p);
    src->data[p] = const_cast<uint8_t*>(frame.visible_data(plane).data());
    src->linesize[p] = frame.stride(plane);
  }
  ff::FramePtr dst = ff::MakeFrame();
  dst->width = width;
  dst->height = height;
  dst->format = AV_PIX_FMT_YUV420P;
  if (av_frame_get_buffer(dst.get(), 32) != 0) {
    return base::unexpected(MediaError(
        ErrorCode::kOutOfMemory, "snapshot buffer allocation failed", {}, {}));
  }
  if (!converter.Convert(*src, dst->data, dst->linesize)) {
    return base::unexpected(
        MediaError(ErrorCode::kInvalidArgument,
                   "converting the frame for a snapshot failed", {}, {}));
  }
  // YUV420P limited-range == YUVJ420P once the flag says full range: the
  // encoder's input format wants the J variant, the bytes are already the
  // limited-range ones the JPEG convention compresses from.
  dst->format = AV_PIX_FMT_YUVJ420P;

  std::vector<uint8_t> jpeg;
  if (const Status st = EncodeToJpeg(ctx.get(), *dst.get(), &jpeg); !st) {
    return st;
  }
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    return base::unexpected(MediaError(
        ErrorCode::kInvalidArgument,
        "cannot open the snapshot file for writing",
        "fopen(" + path + ") failed: " + std::string(std::strerror(errno)),
        "check the directory exists and is writable"));
  }
  const size_t written = std::fwrite(jpeg.data(), 1, jpeg.size(), file);
  std::fclose(file);
  if (written != jpeg.size()) {
    return base::unexpected(MediaError(ErrorCode::kSourceReadFailed,
                                       "short write on the snapshot file",
                                       "wrote " + std::to_string(written) +
                                           " of " + std::to_string(jpeg.size()),
                                       "check disk space"));
  }
  LOG(INFO) << "snapshot written: " << path << " (" << written << " bytes)";
  return Status();
}

}  // namespace avbase::media
