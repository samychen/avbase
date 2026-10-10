// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/ffmpeg_encode_muxer.h"

#include <cstring>
#include <memory>
#include <utility>

#include "base/logging.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace avbase::media {
namespace {

namespace ff = ::avbase::media::ffmpeg;

}  // namespace

struct FFmpegEncodeMuxer::Context {
  AVFormatContext* fmt = nullptr;
  bool header_written = false;
  bool finished = false;
  // G5: container/muxer options passed to avformat_write_header (e.g.
  // "movflags"→"+faststart"). Stored as an AVDictionary pointer so it
  // survives until WriteHeader consumes it.
  AVDictionary* header_options = nullptr;
  // Per-stream source timebase: the units incoming packet pts/dts are in.
  // Audio: 1/sample_rate. Video: the encoder's time_base (1/fps_den).
  // Stored separately from stream->time_base because the container's
  // timebase may differ (avformat_write_header can change it).
  std::vector<AVRational> stream_src_tb;
};

FFmpegEncodeMuxer::FFmpegEncodeMuxer() = default;

FFmpegEncodeMuxer::~FFmpegEncodeMuxer() {
  if (ctx_) {
    if (ctx_->header_written && !ctx_->finished) {
      av_write_trailer(ctx_->fmt);
    }
    if (!(ctx_->fmt->oformat->flags & AVFMT_NOFILE) && ctx_->fmt->pb) {
      avio_closep(&ctx_->fmt->pb);
    }
    avformat_free_context(ctx_->fmt);
    if (ctx_->header_options) {
      av_dict_free(&ctx_->header_options);
    }
  }
}

bool FFmpegEncodeMuxer::Open(
    const std::string& path,
    const std::vector<std::pair<std::string, std::string>>& options) {
  AVFormatContext* fmt = nullptr;
  if (avformat_alloc_output_context2(&fmt, nullptr, nullptr, path.c_str()) <
          0 ||
      !fmt) {
    LOG(ERROR) << "encode muxer: cannot open " << path
               << " (unsupported extension?)";
    return false;
  }
  // For containers that need a file (everything except raw streams like ADTS),
  // open the AVIOContext now: avformat_write_header writes through fmt->pb and
  // crashes if it is null.
  if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
      LOG(ERROR) << "encode muxer: cannot open output file " << path;
      avformat_free_context(fmt);
      return false;
    }
  }
  ctx_ = std::make_unique<Context>();
  ctx_->fmt = fmt;
  // G5: stash the header options so WriteHeader can apply them.
  for (const auto& [key, value] : options) {
    av_dict_set(&ctx_->header_options, key.c_str(), value.c_str(), 0);
  }
  return true;
}

int FFmpegEncodeMuxer::AddAudioStream(const AudioStreamParams& params) {
  if (!ctx_) {
    return -1;
  }
  const AVCodec* codec =
      avcodec_find_encoder_by_name(params.codec_name.c_str());
  AVStream* stream = avformat_new_stream(ctx_->fmt, codec);
  if (!stream) {
    LOG(ERROR) << "encode muxer: new audio stream failed";
    return -1;
  }
  stream->id = static_cast<int>(ctx_->fmt->nb_streams) - 1;
  stream->time_base = AVRational{1, params.sample_rate};
  stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  stream->codecpar->codec_id = codec ? codec->id : AV_CODEC_ID_AAC;
  stream->codecpar->sample_rate = params.sample_rate;
  av_channel_layout_default(&stream->codecpar->ch_layout, params.channels);
  if (!params.extradata.empty()) {
    stream->codecpar->extradata = static_cast<uint8_t*>(
        av_mallocz(params.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(stream->codecpar->extradata, params.extradata.data(),
                params.extradata.size());
    stream->codecpar->extradata_size =
        static_cast<int>(params.extradata.size());
  }
  // Record the source timebase so WritePacket can rescale correctly even if
  // avformat_write_header changes stream->time_base.
  ctx_->stream_src_tb.push_back(AVRational{1, params.sample_rate});
  return stream->id;
}

int FFmpegEncodeMuxer::AddVideoStream(const VideoStreamParams& params) {
  if (!ctx_) {
    return -1;
  }
  const AVCodec* codec =
      avcodec_find_encoder_by_name(params.codec_name.c_str());
  AVStream* stream = avformat_new_stream(ctx_->fmt, codec);
  if (!stream) {
    LOG(ERROR) << "encode muxer: new video stream failed";
    return -1;
  }
  stream->id = static_cast<int>(ctx_->fmt->nb_streams) - 1;
  stream->time_base = AVRational{params.time_base_num, params.time_base_den};
  stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  stream->codecpar->codec_id = codec ? codec->id : AV_CODEC_ID_NONE;
  stream->codecpar->width = params.width;
  stream->codecpar->height = params.height;
  stream->codecpar->format = AV_PIX_FMT_YUV420P;
  if (params.bit_rate > 0) {
    stream->codecpar->bit_rate = params.bit_rate;
  }
  if (!params.extradata.empty()) {
    stream->codecpar->extradata = static_cast<uint8_t*>(
        av_mallocz(params.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(stream->codecpar->extradata, params.extradata.data(),
                params.extradata.size());
    stream->codecpar->extradata_size =
        static_cast<int>(params.extradata.size());
  }
  ctx_->stream_src_tb.push_back(
      AVRational{params.time_base_num, params.time_base_den});
  return stream->id;
}

bool FFmpegEncodeMuxer::WritePacket(int stream_index,
                                    const EncodedPacket& packet) {
  if (!ctx_ || !ctx_->fmt || stream_index < 0 ||
      stream_index >= static_cast<int>(ctx_->fmt->nb_streams)) {
    return false;
  }
  if (!ctx_->header_written) {
    // G5: pass the header options (movflags +faststart, etc.) through.
    if (avformat_write_header(ctx_->fmt, &ctx_->header_options) < 0) {
      LOG(ERROR) << "encode muxer: write_header failed";
      return false;
    }
    ctx_->header_written = true;
    // avformat_write_header may change stream->time_base; the source
    // timebase recorded at AddStream is what packet pts are in.
  }
  AVStream* stream = ctx_->fmt->streams[stream_index];
  // Own the payload: av_interleaved_write_frame takes ownership and unrefs
  // internally, so the caller's buffer must be copied into a real packet.
  ff::PacketPtr pkt = ff::MakePacket();
  if (!pkt) {
    // M10: OOM used to reach av_new_packet with a null packet.
    return false;
  }
  if (av_new_packet(pkt.get(), static_cast<int>(packet.data.size())) < 0) {
    return false;
  }
  std::memcpy(pkt->data, packet.data.data(), packet.data.size());
  pkt->stream_index = stream_index;
  pkt->pts = packet.pts;
  pkt->dts = packet.dts;
  pkt->duration = packet.duration;
  // G1: pass the encoder's flags through instead of hardcoding KEY on every
  // packet. Non-keyframes are now correctly absent from the stss table.
  pkt->flags = packet.flags;
  // G4: carry side data (h264 SPS/PPS in-band, SEI, etc.) through to the
  // muxer so av_interleaved_write_frame can apply them.
  for (const auto& sd : packet.side_data) {
    auto* side = av_packet_new_side_data(
        pkt.get(), static_cast<AVPacketSideDataType>(sd.type), sd.bytes.size());
    if (side) {
      std::memcpy(side, sd.bytes.data(), sd.bytes.size());
    }
  }
  // Rescale from the source timebase (recorded at AddStream) into the
  // container's stream timebase (which write_header may have changed).
  // av_packet_rescale_ts handles pts/dts/duration and skips AV_NOPTS_VALUE.
  const AVRational& src_tb =
      ctx_->stream_src_tb[static_cast<size_t>(stream_index)];
  av_packet_rescale_ts(pkt.get(), src_tb, stream->time_base);
  if (av_interleaved_write_frame(ctx_->fmt, pkt.get()) < 0) {
    LOG(ERROR) << "encode muxer: write_frame failed";
    return false;
  }
  return true;
}

bool FFmpegEncodeMuxer::Finish() {
  if (!ctx_ || !ctx_->fmt || ctx_->finished) {
    return false;
  }
  if (!ctx_->header_written) {
    // Nothing was ever written: still emit a valid (empty) container.
    if (avformat_write_header(ctx_->fmt, &ctx_->header_options) < 0) {
      return false;
    }
    ctx_->header_written = true;
  }
  ctx_->finished = true;
  return av_write_trailer(ctx_->fmt) >= 0;
}

}  // namespace avbase::media
