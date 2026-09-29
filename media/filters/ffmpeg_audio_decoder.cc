// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/ffmpeg_audio_decoder.h"

#include <cstring>
#include <string>
#include <utility>

#include "base/logging.h"
#include "platform/ffmpeg/av_includes.h"
#include "platform/ffmpeg/av_packet_storage.h"
#include "platform/ffmpeg/compat.h"

namespace ff = ::ijkpp::platform::ffmpeg;

namespace ijkpp::media {

struct FFmpegAudioDecoder::Context {
  ~Context() = default;
  ff::CodecCtxPtr codec_ctx;
  ff::FramePtr frame;
};

namespace {

int BytesPerSample(SampleFormat format) {
  switch (format) {
    case SampleFormat::kU8:    return 1;
    case SampleFormat::kS16:
    case SampleFormat::kS16P:  return 2;
    case SampleFormat::kS32:
    case SampleFormat::kS32P:
    case SampleFormat::kF32:
    case SampleFormat::kF32P:  return 4;
    case SampleFormat::kUnknown: return 0;
  }
  return 0;
}

bool IsPlanarFormat(SampleFormat format) {
  return format == SampleFormat::kS16P || format == SampleFormat::kS32P ||
         format == SampleFormat::kF32P;
}

// Copies an AVFrame's samples into one contiguous buffer. Planar frames are
// concatenated channel by channel, matching AudioBuffer::channel_data()'s
// assumption that the payload is channels * (frame_count * bytes_per_sample).
//
// AVFrame::linesize may pad beyond nb_samples * bytes_per_sample; copying the
// padded size would desynchronize every channel after the first, so only the
// real sample bytes are taken.
std::vector<uint8_t> CopyFrameData(const AVFrame* frame, SampleFormat format,
                                   int channels) {
  const int bps = BytesPerSample(format);
  if (bps == 0 || channels <= 0 || frame->nb_samples <= 0) {
    return {};
  }
  const size_t per_channel = static_cast<size_t>(frame->nb_samples) * bps;
  std::vector<uint8_t> out;
  if (IsPlanarFormat(format)) {
    out.resize(per_channel * static_cast<size_t>(channels));
    for (int ch = 0; ch < channels; ++ch) {
      if (!frame->extended_data[ch]) {
        continue;
      }
      std::memcpy(out.data() + per_channel * ch, frame->extended_data[ch],
                  per_channel);
    }
  } else {
    if (!frame->extended_data[0]) {
      return {};
    }
    out.resize(per_channel * static_cast<size_t>(channels));
    std::memcpy(out.data(), frame->extended_data[0], out.size());
  }
  return out;
}

SampleFormat SampleFormatFromAV(int av_format, SampleFormat fallback) {
  switch (av_format) {
    case AV_SAMPLE_FMT_U8:   return SampleFormat::kU8;
    case AV_SAMPLE_FMT_S16:  return SampleFormat::kS16;
    case AV_SAMPLE_FMT_S32:  return SampleFormat::kS32;
    case AV_SAMPLE_FMT_FLT:  return SampleFormat::kF32;
    case AV_SAMPLE_FMT_S16P: return SampleFormat::kS16P;
    case AV_SAMPLE_FMT_S32P: return SampleFormat::kS32P;
    case AV_SAMPLE_FMT_FLTP: return SampleFormat::kF32P;
    default: return fallback;
  }
}

}  // namespace

FFmpegAudioDecoder::FFmpegAudioDecoder() = default;
FFmpegAudioDecoder::~FFmpegAudioDecoder() = default;

DecoderStatus FFmpegAudioDecoder::OpenCodec(const AudioDecoderConfig& config) {
  const AVCodec* codec = avcodec_find_decoder_by_name(config.codec_name.c_str());
  if (!codec) {
    return DecoderStatus(
        DecoderStatus::Codes::kUnsupportedCodec,
        "no FFmpeg audio decoder named '" + config.codec_name + "'");
  }
  ctx_ = std::make_unique<Context>();
  ctx_->codec_ctx.reset(avcodec_alloc_context3(codec));
  if (!ctx_->codec_ctx) {
    return DecoderStatus(DecoderStatus::Codes::kUnknownError,
                         "avcodec_alloc_context3 failed");
  }
  AVCodecContext* codec_ctx = ctx_->codec_ctx.get();
  codec_ctx->sample_rate = config.sample_rate;
  ff::SetChannelLayout(codec_ctx, 0, config.channels);
  if (!config.extra_data.empty()) {
    codec_ctx->extradata = static_cast<uint8_t*>(
        av_mallocz(config.extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (codec_ctx->extradata) {
      std::memcpy(codec_ctx->extradata, config.extra_data.data(),
                  config.extra_data.size());
      codec_ctx->extradata_size = static_cast<int>(config.extra_data.size());
    }
  }
  // AAC and several others need a time base to convert pts; without it every
  // frame comes back with pts == 0 (see bug #26 on the video side).
  codec_ctx->pkt_timebase =
      AVRational{1, config.sample_rate > 0 ? config.sample_rate : 1};
  if (const int ret = avcodec_open2(codec_ctx, codec, nullptr); ret < 0) {
    return DecoderStatus(DecoderStatus::Codes::kUnknownError,
                         "avcodec_open2 failed: " +
                             std::string(ff::AvErrorString(ret)));
  }
  // The decoder's own output format is authoritative: many codecs (AAC in
  // particular) decode to FLTP regardless of what the container advertised.
  sample_format_ = SampleFormatFromAV(codec_ctx->sample_fmt, sample_format_);
  channels_ = ff::ChannelCount(codec_ctx);
  sample_rate_ = codec_ctx->sample_rate;
  if (channels_ > 0) {
    channel_layout_ =
        channels_ == 1 ? ChannelLayout::kMono : ChannelLayout::kStereo;
  }
  return DecoderStatus();
}

void FFmpegAudioDecoder::Initialize(const AudioDecoderConfig& config,
                                    bool /*has_pending_clear*/,
                                    int32_t current_serial, InitCB init_cb,
                                    const OutputCB& output_cb,
                                    const WaitingCB& waiting_cb) {
  serial_ = current_serial;
  output_cb_ = output_cb;
  waiting_cb_ = waiting_cb;
  initialized_ = false;
  ctx_.reset();
  sample_format_ = config.sample_format;
  channel_layout_ = config.channel_layout;
  if (!config.IsValidConfig()) {
    std::move(init_cb).Run(DecoderStatus(
        DecoderStatus::Codes::kUnsupportedConfig,
        "audio config invalid: codec_name='" + config.codec_name +
            "' sample_rate=" + std::to_string(config.sample_rate) +
            " channels=" + std::to_string(config.channels)));
    return;
  }

  if (DecoderStatus status = OpenCodec(config); !status.is_ok()) {
    ctx_.reset();
    std::move(init_cb).Run(std::move(status));
    return;
  }
  ctx_->frame.reset(av_frame_alloc());
  if (!ctx_->frame) {
    ctx_.reset();
    std::move(init_cb).Run(DecoderStatus(DecoderStatus::Codes::kUnknownError,
                                         "av_frame_alloc failed"));
    return;
  }
  initialized_ = true;
  LOG(INFO) << "[audio-dec] initialized codec=" << config.codec_name
            << " rate=" << sample_rate_ << " ch=" << channels_
            << " fmt=" << GetSampleFormatName(sample_format_)
            << " serial=" << serial_;
  std::move(init_cb).Run(DecoderStatus());
}

bool FFmpegAudioDecoder::DecodeAvailableFrames(bool* drained) {
  *drained = false;
  while (true) {
    const int ret = avcodec_receive_frame(ctx_->codec_ctx.get(), ctx_->frame.get());
    if (ret == AVERROR(EAGAIN)) {
      return true;
    }
    if (ret == AVERROR_EOF) {
      *drained = true;
      return true;
    }
    if (ret < 0) {
      LOG(WARNING) << "[audio-dec] receive_frame failed: "
                   << ff::AvErrorString(ret);
      return false;
    }
    const int nb_samples = ctx_->frame->nb_samples;
    const int channels = channels_ > 0 ? channels_
                                       : ff::ChannelCount(ctx_->codec_ctx.get());
    const int sample_rate = sample_rate_ > 0
                                ? sample_rate_
                                : ctx_->codec_ctx->sample_rate;
    std::vector<uint8_t> data =
        CopyFrameData(ctx_->frame.get(), sample_format_, channels);
    const base::TimeDelta ts =
        ctx_->frame->pts == AV_NOPTS_VALUE
            ? media::kNoTimestamp
            : ff::ToTimeDelta(ctx_->frame->pts, ctx_->codec_ctx->pkt_timebase);
    // Duration from the sample count is exact for CBR codecs and avoids
    // frame->duration, which several decoders leave unset.
    const base::TimeDelta dur =
        sample_rate > 0
            ? base::Microseconds(static_cast<int64_t>(nb_samples) * 1000000 /
                                 sample_rate)
            : base::TimeDelta();
    av_frame_unref(ctx_->frame.get());
    if (nb_samples > 0 && data.empty()) {
      // A frame we cannot represent is worse than no frame: the consumer would
      // derive a duration from nb_samples and then read silence.
      LOG(WARNING) << "[audio-dec] dropping unrepresentable frame ("
                   << nb_samples << " samples, " << channels << " ch)";
      continue;
    }
    output_cb_.Run(AudioBuffer::Create(
        sample_format_, channel_layout_, channels, sample_rate, nb_samples, ts,
        dur, serial_, std::move(data)));
  }
}

void FFmpegAudioDecoder::Decode(base::scoped_refptr<DecoderBuffer> buffer,
                                DecodeCB decode_cb) {
  if (!initialized_) {
    std::move(decode_cb).Run(DecoderStatus(
        DecoderStatus::Codes::kNotInitialized,
        "Decode() called before a successful Initialize()"));
    return;
  }
  if (!buffer) {
    output_cb_.Run(AudioBuffer::CreateEOSBuffer());
    std::move(decode_cb).Run(DecoderStatus());
    return;
  }
  if (buffer->IsEndOfStream()) {
    // Drain: an empty packet makes libavcodec emit the frames it was holding
    // back (see bug #16).
    ff::PacketPtr drain(av_packet_alloc());
    if (drain) {
      avcodec_send_packet(ctx_->codec_ctx.get(), drain.get());
    }
    bool drained = false;
    if (!DecodeAvailableFrames(&drained)) {
      std::move(decode_cb).Run(DecoderStatus(
          DecoderStatus::Codes::kDecodeError, "drain failed"));
      return;
    }
    output_cb_.Run(AudioBuffer::CreateEOSBuffer());
    std::move(decode_cb).Run(DecoderStatus());
    return;
  }

  auto* storage = buffer->storage_as<ff::AvPacketStorage>();
  if (!storage || !storage->raw()) {
    // Not an FFmpeg-backed buffer: there is nothing we can hand to libavcodec.
    std::move(decode_cb).Run(DecoderStatus(
        DecoderStatus::Codes::kDecodeError,
        "DecoderBuffer is not backed by an AVPacket; a custom Demuxer must "
        "produce AvPacketStorage for FFmpegAudioDecoder"));
    return;
  }
  // The serial travels with the buffer, not with Initialize(): a seek bumps it
  // and the very next buffer already carries the new value. Stamping from the
  // buffer is what makes stale audio detectable downstream (same rule as
  // FFmpegVideoDecoder).
  serial_ = buffer->serial();
  AVPacket* packet = storage->raw();
  // libavcodec re-times the packet, so restore it afterwards; the storage is
  // shared and may be inspected again by the caller.
  const int64_t saved_pts = packet->pts;
  const int64_t saved_dts = packet->dts;
  const int64_t saved_duration = packet->duration;
  const int ret = avcodec_send_packet(ctx_->codec_ctx.get(), packet);
  packet->pts = saved_pts;
  packet->dts = saved_dts;
  packet->duration = saved_duration;
  if (ret < 0 && ret != AVERROR(EAGAIN)) {
    LOG(WARNING) << "[audio-dec] send_packet failed: " << ff::AvErrorString(ret);
    std::move(decode_cb).Run(
        DecoderStatus(DecoderStatus::Codes::kDecodeError,
                      "avcodec_send_packet: " +
                          std::string(ff::AvErrorString(ret))));
    return;
  }
  bool drained = false;
  if (!DecodeAvailableFrames(&drained)) {
    std::move(decode_cb).Run(DecoderStatus(
        DecoderStatus::Codes::kDecodeError, "avcodec_receive_frame failed"));
    return;
  }
  std::move(decode_cb).Run(DecoderStatus());
}

void FFmpegAudioDecoder::Reset(base::OnceClosure closure) {
  if (initialized_) {
    // Discards any frame the decoder is holding. Without this, the first
    // buffer decoded after a seek can be a pre-seek frame (bug #16).
    avcodec_flush_buffers(ctx_->codec_ctx.get());
  }
  std::move(closure).Run();
}

}  // namespace ijkpp::media
