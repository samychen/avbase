// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/ffmpeg_audio_encoder.h"

#include <cstring>
#include <utility>

#include "base/logging.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace avbase::media {
namespace {

namespace ff = ::avbase::media::ffmpeg;

}  // namespace

struct FFmpegAudioEncoder::Context {
  AVCodecContext* codec = nullptr;
  int channels = 0;
  int64_t next_pts_samples = 0;

  Context() = default;
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  // Same rule as FFmpegVideoEncoder::Context: a failed Initialize() used to
  // leak the context it had already allocated.
  ~Context() {
    if (codec) {
      avcodec_free_context(&codec);
    }
  }
};

FFmpegAudioEncoder::FFmpegAudioEncoder() = default;

FFmpegAudioEncoder::~FFmpegAudioEncoder() = default;

int FFmpegAudioEncoder::frame_size() const {
  return ctx_ && ctx_->codec ? ctx_->codec->frame_size : 0;
}

bool FFmpegAudioEncoder::Initialize(const Params& params) {
  ctx_.reset();
  const std::string name =
      params.codec_name.empty() ? std::string("aac") : params.codec_name;
  const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
  if (!codec) {
    LOG(ERROR) << "audio encoder: no encoder named \"" << name
               << "\" in this FFmpeg build";
    return false;
  }
  auto c = std::make_unique<Context>();
  c->codec = avcodec_alloc_context3(codec);
  if (!c->codec) {
    return false;
  }
  c->channels = params.channels;
  c->codec->sample_rate = params.sample_rate;
  av_channel_layout_default(&c->codec->ch_layout, params.channels);
  // Planar float: what the decode path produces and what every modern audio
  // encoder (aac, libmp3lame, libopus, ac3, flac) accepts. The video encoder
  // hardcodes YUV420P the same way — format negotiation is out of scope here.
  c->codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
  if (params.bit_rate > 0) {
    c->codec->bit_rate = params.bit_rate;
  }
  if (avcodec_open2(c->codec, codec, nullptr) < 0) {
    LOG(ERROR) << "audio encoder: avcodec_open2 failed for \"" << name << "\"";
    avcodec_free_context(&c->codec);
    return false;
  }
  if (c->codec->extradata && c->codec->extradata_size > 0) {
    ctx_extradata_.assign(c->codec->extradata,
                          c->codec->extradata + c->codec->extradata_size);
  }
  ctx_ = std::move(c);
  return true;
}

bool FFmpegAudioEncoder::Encode(base::scoped_refptr<AudioBuffer> in,
                                std::vector<EncodedPacket>* out) {
  if (!ctx_ || !out) {
    return false;
  }
  AVFrame* frame = av_frame_alloc();
  frame->format = ctx_->codec->sample_fmt;
  frame->sample_rate = ctx_->codec->sample_rate;
  frame->nb_samples = in->frame_count();
  av_channel_layout_copy(&frame->ch_layout, &ctx_->codec->ch_layout);
  frame->pts = ctx_next_pts_samples_;
  for (int ch = 0; ch < ctx_->channels; ++ch) {
    frame->data[ch] = const_cast<uint8_t*>(in->channel_data(ch).data());
  }
  const int send = avcodec_send_frame(ctx_->codec, frame);
  // Read everything the error path wants to report BEFORE freeing: av_frame
  // is gone after this line, and logging its fields later is a use-after-free
  // that crashes the very diagnostic meant to explain the failure.
  const int sent_nb_samples = frame->nb_samples;
  const int sent_rate = frame->sample_rate;
  const int sent_channels = frame->ch_layout.nb_channels;
  av_frame_free(&frame);
  if (send < 0) {
    char msg[AV_ERROR_MAX_STRING_SIZE] = {};
    LOG(ERROR) << "audio encoder: send_frame failed: "
               << av_make_error_string(msg, sizeof(msg), send)
               << " (nb_samples=" << sent_nb_samples << " rate=" << sent_rate
               << " ch=" << sent_channels << ")";
    return false;
  }
  ctx_next_pts_samples_ += in->frame_count();

  while (true) {
    ff::PacketPtr packet(av_packet_alloc());
    const int got = avcodec_receive_packet(ctx_->codec, packet.get());
    if (got < 0) {
      break;  // EAGAIN: needs more input.
    }
    EncodedPacket ep;
    ep.data.assign(packet->data, packet->data + packet->size);
    ep.pts = packet->pts;
    ep.dts = packet->dts;
    ep.duration = packet->duration;
    ep.flags = packet->flags;
    out->push_back(std::move(ep));
  }
  return true;
}

bool FFmpegAudioEncoder::Flush(std::vector<EncodedPacket>* out) {
  if (!ctx_ || !out) {
    return false;
  }
  if (avcodec_send_frame(ctx_->codec, nullptr) < 0) {
    return false;
  }
  while (true) {
    ff::PacketPtr packet(av_packet_alloc());
    const int got = avcodec_receive_packet(ctx_->codec, packet.get());
    if (got < 0) {
      break;
    }
    EncodedPacket ep;
    ep.data.assign(packet->data, packet->data + packet->size);
    ep.pts = packet->pts;
    ep.dts = packet->dts;
    ep.duration = packet->duration;
    ep.flags = packet->flags;
    out->push_back(std::move(ep));
  }
  return true;
}

}  // namespace avbase::media
