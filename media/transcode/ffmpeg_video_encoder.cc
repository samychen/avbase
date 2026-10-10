// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/ffmpeg_video_encoder.h"

#include <cstring>
#include <utility>

#include "base/logging.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"
#include "media/ffmpeg/video_convert.h"

namespace avbase::media {
namespace {

namespace ff = ::avbase::media::ffmpeg;

}  // namespace

struct FFmpegVideoEncoder::Context {
  AVCodecContext* codec = nullptr;
  AVCodecParameters* par = nullptr;
  AVPixelFormat in_fmt = AV_PIX_FMT_YUV420P;   // Encoder's input format.
  AVPixelFormat dst_fmt = AV_PIX_FMT_YUV420P;  // Converted frame format.
  ff::VideoConverter converter;
  ff::FramePtr converted;

  Context() = default;
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  // Owns the libav objects. Before this existed, every Initialize() that
  // returned false leaked its half-built context — which is exactly the
  // path a caller walks when it tries candidate encoders in order and the
  // first one is missing from the build.
  ~Context() {
    if (par) {
      avcodec_parameters_free(&par);
    }
    if (codec) {
      avcodec_free_context(&codec);
    }
  }

  // Drains queued packets from the encoder into EncodedPacket objects that
  // carry pts/dts/flags/duration and side_data (h264 SPS/PPS in-band, etc.)
  // so the muxer receives the same information av_interleaved_write_frame
  // would get from a raw AVPacket.
  void DrainPackets(std::vector<EncodedPacket>* out) {
    while (true) {
      ff::PacketPtr packet(av_packet_alloc());
      const int got = avcodec_receive_packet(codec, packet.get());
      if (got < 0) {
        break;
      }
      EncodedPacket ep;
      ep.data.assign(packet->data, packet->data + packet->size);
      ep.pts = packet->pts;
      ep.dts = packet->dts;
      ep.duration = packet->duration;
      ep.flags = packet->flags;
      // Carry side data through so h264's SPS/PPS (AV_PKT_DATA_NEW_EXTRADATA)
      // and other per-packet metadata survive into the muxer.
      for (int i = 0; i < packet->side_data_elems; ++i) {
        const auto& sd = packet->side_data[i];
        EncodedPacket::SideData side;
        side.type = sd.type;
        side.bytes.assign(sd.data, sd.data + sd.size);
        ep.side_data.push_back(std::move(side));
      }
      out->push_back(std::move(ep));
    }
  }
};

FFmpegVideoEncoder::FFmpegVideoEncoder() = default;

FFmpegVideoEncoder::~FFmpegVideoEncoder() = default;

bool FFmpegVideoEncoder::Initialize(const Params& params) {
  // Replacing an existing encoder must free it; unique_ptr<Context> alone
  // used to drop it without freeing once ~Context existed.
  ctx_.reset();
  const AVCodec* codec =
      avcodec_find_encoder_by_name(params.codec_name.c_str());
  if (!codec) {
    LOG(ERROR) << "video encoder: \"" << params.codec_name
               << "\" not in this FFmpeg build";
    return false;
  }
  auto c = std::make_unique<Context>();
  c->codec = avcodec_alloc_context3(codec);
  if (!c->codec) {
    return false;
  }
  c->codec->width = params.width;
  c->codec->height = params.height;
  // G6: time_base and framerate come from the caller's fps rational instead
  // of the hardcoded 30. ffmpeg.c sets time_base = av_inv_q(frame_rate),
  // i.e. {fps_den, fps_num}: one tick = one frame interval (30 fps -> 1/30 s
  // per tick). The old {1, fps_den} treated the fps DENOMINATOR as the rate,
  // giving {1,1} -- one tick per SECOND -- for every integer-rate source, so
  // microseconds fed as pts became seconds.
  c->codec->time_base = AVRational{params.fps_den, params.fps_num};
  c->codec->framerate = AVRational{params.fps_num, params.fps_den};
  if (params.bit_rate > 0) {
    c->codec->bit_rate = params.bit_rate;
  }
  if (params.gop > 0) {
    c->codec->gop_size = params.gop;
  }
  if (!params.preset.empty() &&
      av_opt_set(c->codec->priv_data, "preset", params.preset.c_str(), 0) < 0) {
    LOG(WARNING) << "video encoder: unknown preset \"" << params.preset
                 << "\"; using the encoder default";
  }
  if (params.crf >= 0 &&
      av_opt_set_int(c->codec->priv_data, "crf", params.crf, 0) < 0) {
    LOG(WARNING) << "video encoder: crf not supported by this encoder";
  }

  // The conversion target: encoders whose native input is full-range J
  // variants (mjpeg) get YUVJ420P; everything else (libx264) gets YUV420P.
  const bool full_range = strcmp(codec->name, "mjpeg") == 0;
  c->dst_fmt = full_range ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P;
  c->codec->pix_fmt = c->dst_fmt;
  c->codec->color_range = full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;

  if (avcodec_open2(c->codec, codec, nullptr) < 0) {
    LOG(ERROR) << "video encoder: avcodec_open2 failed for \""
               << params.codec_name << "\"";
    avcodec_free_context(&c->codec);
    return false;
  }

  // The frames coming in are avbase's decode output: I420 (limited range).
  // Normalize to the encoder's input format through the shared converter.
  c->in_fmt = ff::AvPixelFormatFromVideoFormat(VideoFormat::kI420);
  if (!c->converter.Configure(params.width, params.height, c->in_fmt,
                              params.width, params.height, c->dst_fmt)) {
    LOG(ERROR) << "video encoder: conversion setup failed";
    avcodec_free_context(&c->codec);
    return false;
  }
  c->converted = ff::FramePtr(av_frame_alloc());
  c->converted->width = params.width;
  c->converted->height = params.height;
  c->converted->format = c->dst_fmt;
  if (av_frame_get_buffer(c->converted.get(), 32) != 0) {
    LOG(ERROR) << "video encoder: buffer alloc failed";
    avcodec_free_context(&c->codec);
    return false;
  }
  ctx_ = std::move(c);
  return true;
}

const AVCodecParameters* FFmpegVideoEncoder::codec_parameters() const {
  // Filled from the codec context by avcodec_parameters_from_context on
  // first call; EncodeMuxer copies it into its stream. Owned by the encoder.
  if (!ctx_) {
    return nullptr;
  }
  if (!ctx_->par) {
    ctx_->par = avcodec_parameters_alloc();
    if (avcodec_parameters_from_context(ctx_->par, ctx_->codec) < 0) {
      avcodec_parameters_free(&ctx_->par);
    }
  }
  return ctx_->par;
}

AVRational FFmpegVideoEncoder::time_base() const {
  if (!ctx_) {
    return AVRational{1, 30};
  }
  return ctx_->codec->time_base;
}

bool FFmpegVideoEncoder::Encode(base::scoped_refptr<VideoFrame> in,
                                std::vector<EncodedPacket>* out) {
  if (!ctx_ || !out) {
    return false;
  }
  // View over the source planes, then convert into the reused target frame.
  ff::FramePtr src(av_frame_alloc());
  if (!src) {
    // M10: OOM used to null-deref on the field writes right below.
    LOG(ERROR) << "transcode: av_frame_alloc failed in the video encoder";
    return false;
  }
  src->width = ctx_->codec->width;
  src->height = ctx_->codec->height;
  src->format = ctx_->in_fmt;
  for (int p = 0; p < VideoFrame::kMaxPlanes; ++p) {
    const auto plane = static_cast<VideoFrame::Plane>(p);
    src->data[p] = const_cast<uint8_t*>(in->visible_data(plane).data());
    src->linesize[p] = in->stride(plane);
  }
  if (!ctx_->converter.Convert(*src, ctx_->converted->data,
                               ctx_->converted->linesize)) {
    LOG(ERROR) << "video encoder: conversion failed";
    return false;
  }
  // Range/matrix flow through from the frame so colours survive the trip.
  switch (in->color_space().matrix) {
  case ColorMatrix::kBT709:
    ctx_->converted->colorspace = AVCOL_SPC_BT709;
    break;
  case ColorMatrix::kBT2020Ncl:
    ctx_->converted->colorspace = AVCOL_SPC_BT2020_NCL;
    break;
  case ColorMatrix::kSMPTE170M:
    ctx_->converted->colorspace = AVCOL_SPC_SMPTE170M;
    break;
  default:
    ctx_->converted->colorspace = AVCOL_SPC_BT470BG;
    break;
  }
  ctx_->converted->color_range = in->color_space().range == ColorRange::kFull
                                     ? AVCOL_RANGE_JPEG
                                     : AVCOL_RANGE_MPEG;
  // The frame's timestamp is microseconds (avbase VideoFrame); the encoder's
  // time_base ticks are frame intervals. Rescale instead of feeding raw
  // microseconds: under the old {1, fps_den} time_base a 33333 us frame
  // interval became 33333 SECONDS of timeline.
  ctx_->converted->pts =
      ff::FromTimeDelta(in->timestamp(), ctx_->codec->time_base);

  const int send = avcodec_send_frame(ctx_->codec, ctx_->converted.get());
  if (send < 0) {
    LOG(ERROR) << "video encoder: send_frame failed";
    return false;
  }
  ctx_->DrainPackets(out);
  return true;
}

bool FFmpegVideoEncoder::Flush(std::vector<EncodedPacket>* out) {
  if (!ctx_ || !out) {
    return false;
  }
  if (avcodec_send_frame(ctx_->codec, nullptr) < 0) {
    return false;
  }
  ctx_->DrainPackets(out);
  return true;
}

}  // namespace avbase::media
