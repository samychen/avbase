// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_demuxer_configs.h"

extern "C" {
#include <libavutil/display.h>
}

#include <cmath>
#include <string>

#include "media/base/decoder_config.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace avbase::media {

namespace ff = ::avbase::media::ffmpeg;

VideoDecoderConfig MakeVideoDecoderConfig(const AVStream* stream) {
  VideoDecoderConfig config;
  const AVCodecParameters* par = stream->codecpar;
  const AVCodec* codec = avcodec_find_decoder(par->codec_id);
  config.codec_name = codec ? codec->name : "unknown";
  config.codec = VideoCodecFromName(config.codec_name);
  config.coded_size = Size{par->width, par->height};
  if (par->sample_aspect_ratio.num > 0 && par->sample_aspect_ratio.den > 0) {
    config.sar =
        Rational{par->sample_aspect_ratio.num, par->sample_aspect_ratio.den};
    // natural_size is the SAR-applied display size. Rounding matches FFmpeg's
    // av_reduce so a 720x576 anamorphic stream reports 1024x576.
    const int64_t w = static_cast<int64_t>(par->width) * config.sar.num;
    const int64_t d = config.sar.den;
    config.natural_size = Size{static_cast<int>((w + d / 2) / d), par->height};
  } else {
    config.natural_size = config.coded_size;
  }
  config.frame_rate =
      Rational{stream->avg_frame_rate.num, stream->avg_frame_rate.den};
  // The decoder cannot interpret AVFrame::pts without this (see
  // VideoDecoderConfig::time_base).
  config.time_base = Rational{stream->time_base.num, stream->time_base.den};
  config.avg_frame_rate = config.frame_rate;
  config.bit_rate = par->bit_rate > 0 ? par->bit_rate : 0;
  config.profile = par->profile >= 0 ? std::to_string(par->profile) : "";
  config.level = par->level >= 0 ? std::to_string(par->level) : "";
  if (par->extradata && par->extradata_size > 0) {
    config.extra_data.assign(par->extradata,
                             par->extradata +
                                 static_cast<size_t>(par->extradata_size));
  }
  // Rotation lives in a display-matrix side-data entry, not in the codec
  // parameters. ijkplayer reads it in three separate places.
  size_t matrix_size = 0;
  const uint8_t* matrix =
      avbase_stream_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, &matrix_size);
  if (matrix && matrix_size >= 9 * sizeof(int32_t)) {
    const double rotation =
        av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix));
    if (!std::isnan(rotation)) {
      int degrees = static_cast<int>(-rotation) % 360;
      if (degrees < 0) {
        degrees += 360;
      }
      config.rotation = degrees;
    }
  }
  return config;
}

AudioDecoderConfig MakeAudioDecoderConfig(const AVStream* stream) {
  AudioDecoderConfig config;
  const AVCodecParameters* par = stream->codecpar;
  const AVCodec* codec = avcodec_find_decoder(par->codec_id);
  config.codec_name = codec ? codec->name : "unknown";
  config.codec = AudioCodecFromName(config.codec_name);
  config.sample_rate = par->sample_rate;
  config.channels = ff::ChannelCount(par);
  switch (config.channels) {
  case 1:
    config.channel_layout = ChannelLayout::kMono;
    break;
  case 2:
    config.channel_layout = ChannelLayout::kStereo;
    break;
  case 6:
    config.channel_layout = ChannelLayout::k5_1;
    break;
  case 8:
    config.channel_layout = ChannelLayout::k7_1;
    break;
  default:
    config.channel_layout = ff::ChannelLayoutMask(par)
                                ? ChannelLayout::kDiscrete
                                : ChannelLayout::kNone;
    break;
  }
  switch (par->format) {
  case AV_SAMPLE_FMT_U8:
    config.sample_format = SampleFormat::kU8;
    break;
  case AV_SAMPLE_FMT_S16:
    config.sample_format = SampleFormat::kS16;
    break;
  case AV_SAMPLE_FMT_S32:
    config.sample_format = SampleFormat::kS32;
    break;
  case AV_SAMPLE_FMT_FLT:
    config.sample_format = SampleFormat::kF32;
    break;
  case AV_SAMPLE_FMT_S16P:
    config.sample_format = SampleFormat::kS16P;
    break;
  case AV_SAMPLE_FMT_S32P:
    config.sample_format = SampleFormat::kS32P;
    break;
  case AV_SAMPLE_FMT_FLTP:
    config.sample_format = SampleFormat::kF32P;
    break;
  default:
    config.sample_format = SampleFormat::kUnknown;
    break;
  }
  config.bit_rate = par->bit_rate > 0 ? par->bit_rate : 0;
  config.codec_delay_frames = par->initial_padding;
  config.seek_preroll_frames = par->seek_preroll;
  if (par->extradata && par->extradata_size > 0) {
    config.extra_data.assign(par->extradata,
                             par->extradata +
                                 static_cast<size_t>(par->extradata_size));
  }
  return config;
}

TextDecoderConfig MakeTextDecoderConfig(const AVStream* stream) {
  TextDecoderConfig config;
  const AVCodecParameters* par = stream->codecpar;
  const AVCodec* codec = avcodec_find_decoder(par->codec_id);
  config.codec_name = codec ? codec->name : "unknown";
  if (par->extradata && par->extradata_size > 0) {
    config.extra_data.assign(par->extradata,
                             par->extradata +
                                 static_cast<size_t>(par->extradata_size));
  }
  return config;
}

}  // namespace avbase::media
