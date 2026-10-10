// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/ffmpeg_transcode_streams.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "media/base/decoder_config.h"  // VideoCodecFromName
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace avbase::media {
namespace {

namespace ff = ::avbase::media::ffmpeg;

// Extracts extradata as a vector (padded).
std::vector<uint8_t> CopyExtradata(const uint8_t* data, int size) {
  if (!data || size <= 0) {
    return {};
  }
  return {data, data + static_cast<size_t>(size)};
}

// Opens a decoder context from an AVStream's codecpar.
bool OpenDecoder(AVStream* stream, CodecCtxPtr* out) {
  const AVCodec* dec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!dec) {
    LOG(ERROR) << "transcode: no decoder for codec_id="
               << static_cast<int>(stream->codecpar->codec_id);
    return false;
  }
  AVCodecContext* ctx = avcodec_alloc_context3(dec);
  if (!ctx) {
    return false;
  }
  if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0) {
    avcodec_free_context(&ctx);
    return false;
  }
  ctx->pkt_timebase = stream->time_base;
  if (avcodec_open2(ctx, dec, nullptr) < 0) {
    avcodec_free_context(&ctx);
    LOG(ERROR) << "transcode: avcodec_open2 failed for decoder";
    return false;
  }
  out->reset(ctx);
  return true;
}

AVStream* StreamAt(AVFormatContext* ctx, int index) {
  return ctx->streams[static_cast<unsigned>(index)];
}

}  // namespace

void CodecCtxDeleter::operator()(AVCodecContext* p) const {
  if (p) {
    avcodec_free_context(&p);
  }
}

void AvFrameDeleter::operator()(AVFrame* p) const {
  if (p) {
    av_frame_free(&p);
  }
}

// AvFrameToAudioBuffer (and the audio resampling it feeds) live in
// ffmpeg_transcode_audio.cc.

base::scoped_refptr<VideoFrame> AvFrameToVideoFrame(AVFrame* frame, int width,
                                                    int height,
                                                    AVRational pts_time_base) {
  const Size size{width, height};
  // ff::ToTimeDelta maps AV_NOPTS_VALUE to kNoTimestamp (the trap recorded in
  // this header) instead of leaking a raw rescale of the sentinel, matching
  // how the playback-side decoders timestamp their frames.
  const base::TimeDelta timestamp = ff::ToTimeDelta(frame->pts, pts_time_base);
  const base::TimeDelta dur =
      frame->duration > 0 ? ff::ToTimeDelta(frame->duration, pts_time_base)
                          : base::Milliseconds(33);
  auto vf = VideoFrame::CreateBlackFrame(VideoFormat::kI420, size, size,
                                         Rational{1, 1}, timestamp, dur, 0);
  if (!vf) {
    return nullptr;
  }
  // Copy Y plane.
  auto y_span = vf->mutable_data(VideoFrame::kYPlane);
  for (int y = 0; y < height; ++y) {
    std::memcpy(y_span.data() +
                    static_cast<size_t>(y) * static_cast<size_t>(width),
                frame->data[0] + static_cast<ptrdiff_t>(y) * frame->linesize[0],
                static_cast<size_t>(width));
  }
  // Copy U/V planes (half width/height for I420).
  const int uv_w = width / 2;
  const int uv_h = height / 2;
  auto u_span = vf->mutable_data(VideoFrame::kUPlane);
  auto v_span = vf->mutable_data(VideoFrame::kVPlane);
  for (int y = 0; y < uv_h; ++y) {
    std::memcpy(u_span.data() +
                    static_cast<size_t>(y) * static_cast<size_t>(uv_w),
                frame->data[1] + static_cast<ptrdiff_t>(y) * frame->linesize[1],
                static_cast<size_t>(uv_w));
    std::memcpy(v_span.data() +
                    static_cast<size_t>(y) * static_cast<size_t>(uv_w),
                frame->data[2] + static_cast<ptrdiff_t>(y) * frame->linesize[2],
                static_cast<size_t>(uv_w));
  }
  return vf;
}

int64_t TimelineOrigin(bool* seen, int64_t* origin, int64_t ts) {
  if (!*seen && ts != AV_NOPTS_VALUE) {
    *origin = ts;
    *seen = true;
  }
  return *origin;
}

int64_t ShiftTs(int64_t ts, int64_t origin) {
  return ts == AV_NOPTS_VALUE ? ts : ts - origin;
}

// ScratchFrame / DrainWholeFrames (and FramesForEncoder / FlushResampler)
// live in ffmpeg_transcode_audio.cc. The Swr reseampling itself is in
// media/ffmpeg/audio_convert.h (AudioConverter), lazily configured there.

// (FramesForEncoder / FlushResampler are defined in ffmpeg_transcode_audio.cc.)

void RescaleTimestamps(EncodedPacket* ep, int in_num, int in_den, int out_num,
                       int out_den) {
  // An unset timebase means "unknown", and dividing by it is not a
  // meaningful way to discover that. Leave the packet alone instead.
  if (in_num <= 0 || in_den <= 0 || out_num <= 0 || out_den <= 0) {
    return;
  }
  const AVRational in{in_num, in_den};
  const AVRational out{out_num, out_den};
  if (in_num == out_num && in_den == out_den) {
    return;  // Same units: nothing to convert (and no rounding to introduce).
  }
  if (ep->pts != AV_NOPTS_VALUE) {
    ep->pts = av_rescale_q(ep->pts, in, out);
  }
  if (ep->dts != AV_NOPTS_VALUE) {
    ep->dts = av_rescale_q(ep->dts, in, out);
  }
  if (ep->duration > 0) {
    ep->duration = av_rescale_q(ep->duration, in, out);
  }
}

Status PrepareAudioStream(AVFormatContext* in_ctx, int audio_stream_idx,
                          const TranscodeParams& params,
                          FFmpegEncodeMuxer* muxer, AudioState* out) {
  out->copy = params.audio_codec.codec == "copy";
  if (!out->copy) {
    // Open decoder.
    AVStream* as = StreamAt(in_ctx, audio_stream_idx);
    if (!OpenDecoder(as, &out->decoder)) {
      return Err(ErrorCode::kDecoderOpenFailed, "audio decoder open failed",
                 as->codecpar->codec_id ? "codec not supported" : "",
                 "try setting audio_codec to \"copy\"");
    }
    out->in_sample_rate = as->codecpar->sample_rate;
    out->in_channels = as->codecpar->ch_layout.nb_channels;
    out->out_sample_rate = params.audio_codec.sample_rate > 0
                               ? params.audio_codec.sample_rate
                               : out->in_sample_rate;
    out->out_channels = params.audio_codec.channels > 0
                            ? params.audio_codec.channels
                            : out->in_channels;
    // Initialize encoder.
    FFmpegAudioEncoder::Params enc_params;
    enc_params.codec_name = params.audio_codec.codec;
    enc_params.sample_rate = out->out_sample_rate;
    enc_params.channels = out->out_channels;
    enc_params.bit_rate = params.audio_codec.bit_rate;
    if (!out->encoder.Initialize(enc_params)) {
      return Err(ErrorCode::kDecoderOpenFailed, "audio encoder init failed",
                 "codec=" + enc_params.codec_name,
                 "check the encoder is built into FFmpeg and the sample_rate / "
                 "channels are supported");
    }
    // Add muxer stream. |codec_name| is the RESOLVED name, not the one asked
    // for — asking for "libopus" and writing "aac" into the container header
    // would describe a stream that isn't there.
    FFmpegEncodeMuxer::AudioStreamParams ms;
    ms.sample_rate = out->out_sample_rate;
    ms.channels = out->out_channels;
    ms.codec_name = enc_params.codec_name;
    ms.extradata = out->encoder.extradata();
    out->muxer_stream_index = muxer->AddAudioStream(ms);
    if (out->muxer_stream_index < 0) {
      return Err(ErrorCode::kInvalidState, "cannot add audio stream to muxer",
                 {}, {});
    }
    // Re-encoded packets carry the encoder's own timestamps, already in
    // 1/sample_rate — the same units the muxer expects.
    out->in_tb_num = 1;
    out->in_tb_den = out->out_sample_rate;
    out->out_tb_num = out->in_tb_num;
    out->out_tb_den = out->in_tb_den;
    return OkStatus();
  }

  // Copy mode: add a stream mirroring the input.
  AVStream* as = StreamAt(in_ctx, audio_stream_idx);
  FFmpegEncodeMuxer::AudioStreamParams ms;
  ms.sample_rate = as->codecpar->sample_rate;
  ms.channels = as->codecpar->ch_layout.nb_channels;
  const AVCodec* enc = avcodec_find_encoder(as->codecpar->codec_id);
  ms.codec_name = enc ? enc->name : "aac";
  ms.extradata =
      CopyExtradata(as->codecpar->extradata, as->codecpar->extradata_size);
  // Copy forwards the input's packets verbatim, so its timestamps arrive in
  // the input container's units. The muxer reads them as 1/sample_rate.
  out->in_tb_num = as->time_base.num;
  out->in_tb_den = as->time_base.den;
  if (ms.sample_rate > 0) {
    out->out_tb_num = 1;
    out->out_tb_den = ms.sample_rate;
  } else {
    // No usable sample rate: leave the timestamps alone rather than divide
    // by zero. Such a stream is broken anyway and will fail downstream.
    out->out_tb_num = out->in_tb_num;
    out->out_tb_den = out->in_tb_den;
  }
  out->muxer_stream_index = muxer->AddAudioStream(ms);
  if (out->muxer_stream_index < 0) {
    return Err(ErrorCode::kInvalidState,
               "cannot add audio copy stream to muxer", {}, {});
  }
  return OkStatus();
}

Status PrepareVideoStream(AVFormatContext* in_ctx, int video_stream_idx,
                          const TranscodeParams& params,
                          FFmpegEncodeMuxer* muxer, VideoState* out) {
  out->copy = params.video_codec.codec == "copy";
  if (out->copy) {
    AVStream* vs = StreamAt(in_ctx, video_stream_idx);
    FFmpegEncodeMuxer::VideoStreamParams ms;
    const AVCodec* enc = avcodec_find_encoder(vs->codecpar->codec_id);
    ms.codec_name = enc ? enc->name : "libx264";
    ms.width = vs->codecpar->width;
    ms.height = vs->codecpar->height;
    ms.bit_rate = static_cast<int>(vs->codecpar->bit_rate);
    ms.time_base_num = vs->time_base.num;
    ms.time_base_den = vs->time_base.den;
    ms.extradata =
        CopyExtradata(vs->codecpar->extradata, vs->codecpar->extradata_size);
    // The output stream is declared with the input's timebase, so copy is
    // already in the right units. Recorded explicitly rather than assumed,
    // because the audio side of the same job cannot make that assumption.
    out->in_tb_num = vs->time_base.num;
    out->in_tb_den = vs->time_base.den;
    out->out_tb_num = ms.time_base_num;
    out->out_tb_den = ms.time_base_den;
    out->muxer_stream_index = muxer->AddVideoStream(ms);
    if (out->muxer_stream_index < 0) {
      return Err(ErrorCode::kInvalidState,
                 "cannot add video copy stream to muxer", {}, {});
    }
    return OkStatus();
  }

  AVStream* vs = StreamAt(in_ctx, video_stream_idx);
  if (!OpenDecoder(vs, &out->decoder)) {
    return Err(ErrorCode::kDecoderOpenFailed, "video decoder open failed", {},
               "try setting video_codec to \"copy\"");
  }
  out->in_width = vs->codecpar->width;
  out->in_height = vs->codecpar->height;
  // Input fps.
  if (vs->avg_frame_rate.den > 0 && vs->avg_frame_rate.num > 0) {
    out->in_fps_num = vs->avg_frame_rate.num;
    out->in_fps_den = vs->avg_frame_rate.den;
  } else if (vs->r_frame_rate.den > 0 && vs->r_frame_rate.num > 0) {
    out->in_fps_num = vs->r_frame_rate.num;
    out->in_fps_den = vs->r_frame_rate.den;
  } else {
    out->in_fps_num = 30;
    out->in_fps_den = 1;
  }
  out->out_width =
      params.video_codec.width > 0 ? params.video_codec.width : out->in_width;
  out->out_height = params.video_codec.height > 0 ? params.video_codec.height
                                                  : out->in_height;
  out->out_fps_num = params.video_codec.fps_num > 0 ? params.video_codec.fps_num
                                                    : out->in_fps_num;
  out->out_fps_den = params.video_codec.fps_den > 0 ? params.video_codec.fps_den
                                                    : out->in_fps_den;

  // --- Resolve the encoder through the factory list (E5) ---
  // |codec| names a family ("libx264", "mjpeg"); the factory turns it into
  // whatever this build actually has for that family, trying the list in
  // priority order. Hardware factories only survive selection when
  // |prefer_hardware| is set, so the default path keeps working on machines
  // with no usable hardware encoder.
  std::string encoder_name = params.video_codec.codec;
  std::vector<std::string> candidates;
  if (params.video_codec.use_encoder_factory) {
    const VideoCodec family = VideoCodecFromName(params.video_codec.codec);
    std::vector<base::scoped_refptr<ff::VideoEncoderFactory>> factories =
        params.video_codec.factories;
    if (factories.empty()) {
      factories = ff::CreateVideoEncoderFactories();
    }
    const HwCodecMask mask = params.video_codec.prefer_hardware
                                 ? params.video_codec.hw_mask
                                 : static_cast<HwCodecMask>(0);
    for (const auto& f : ff::SelectVideoEncoder(factories, family, mask)) {
      const std::string name = f->EncoderNameFor(family);
      if (!name.empty()) {
        candidates.push_back(name);
      }
    }
  } else {
    candidates.push_back(params.video_codec.codec);
  }
  if (candidates.empty()) {
    candidates.push_back(params.video_codec.codec);
    LOG(WARNING) << "transcode: no video encoder factory handles \""
                 << params.video_codec.codec << "\"; using the name as given";
  }

  // Initialize the encoder, walking candidates in priority order: a missing
  // encoder is a property of the build, not a reason to abandon the rest.
  FFmpegVideoEncoder::Params enc_params;
  enc_params.width = out->out_width;
  enc_params.height = out->out_height;
  enc_params.bit_rate = params.video_codec.bit_rate;
  enc_params.crf = params.video_codec.crf;
  enc_params.gop = params.video_codec.gop;
  enc_params.preset = params.video_codec.preset;
  enc_params.fps_num = out->out_fps_num;
  enc_params.fps_den = out->out_fps_den;
  bool encoder_ready = false;
  for (const auto& name : candidates) {
    enc_params.codec_name = name;
    if (out->encoder.Initialize(enc_params)) {
      encoder_name = name;
      encoder_ready = true;
      break;
    }
    LOG(WARNING) << "transcode: video encoder \"" << name
                 << "\" unavailable, trying the next candidate";
  }
  if (!encoder_ready) {
    return Err(ErrorCode::kDecoderOpenFailed, "video encoder init failed",
               "codec=" + params.video_codec.codec,
               "ensure one of its encoders is built into FFmpeg");
  }

  // Add muxer stream. |encoder_name| is the RESOLVED name, not the one
  // asked for — asking for "mjpeg" and writing "libx264" into the container
  // header would describe a stream that isn't there.
  FFmpegEncodeMuxer::VideoStreamParams ms;
  ms.codec_name = encoder_name;
  ms.width = out->out_width;
  ms.height = out->out_height;
  ms.bit_rate = params.video_codec.bit_rate;
  const AVRational tb = out->encoder.time_base();
  ms.time_base_num = tb.num;
  ms.time_base_den = tb.den;
  out->in_tb_num = tb.num;
  out->in_tb_den = tb.den;
  out->out_tb_num = tb.num;
  out->out_tb_den = tb.den;
  const AVCodecParameters* cp = out->encoder.codec_parameters();
  if (cp && cp->extradata && cp->extradata_size > 0) {
    ms.extradata = CopyExtradata(cp->extradata, cp->extradata_size);
  }
  out->muxer_stream_index = muxer->AddVideoStream(ms);
  if (out->muxer_stream_index < 0) {
    return Err(ErrorCode::kInvalidState, "cannot add video stream to muxer", {},
               {});
  }
  return OkStatus();
}

}  // namespace avbase::media
