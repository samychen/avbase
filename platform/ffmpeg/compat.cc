// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/ffmpeg/compat.h"

#include <cmath>
#include <utility>

#include "base/check.h"
#include "media/base/media_constants.h"

namespace ijkpp::platform::ffmpeg {

int ChannelCount(const AVCodecContext* ctx) {
#if IJKPP_FFMPEG_HAS_CHANNEL_LAYOUT
  return ctx->ch_layout.nb_channels;
#else
  return ctx->channels;
#endif
}

uint64_t ChannelLayoutMask(const AVCodecContext* ctx) {
#if IJKPP_FFMPEG_HAS_CHANNEL_LAYOUT
  uint64_t mask = 0;
  if (ctx->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) {
    mask = ctx->ch_layout.u.mask;
  }
  if (!mask) {
    AVChannelLayout def{};
    av_channel_layout_default(&def, ctx->ch_layout.nb_channels);
    if (def.order == AV_CHANNEL_ORDER_NATIVE) {
      mask = def.u.mask;
    }
    av_channel_layout_uninit(&def);
  }
  return mask;
#else
  return ctx->channel_layout
             ? ctx->channel_layout
             : static_cast<uint64_t>(av_get_default_channel_layout(ctx->channels));
#endif
}

void SetChannelLayout(AVCodecContext* ctx, uint64_t mask, int channels) {
#if IJKPP_FFMPEG_HAS_CHANNEL_LAYOUT
  if (mask) {
    av_channel_layout_from_mask(&ctx->ch_layout, mask);
  } else {
    av_channel_layout_default(&ctx->ch_layout, channels);
  }
#else
  ctx->channel_layout = mask ? mask : av_get_default_channel_layout(channels);
  ctx->channels = channels;
#endif
}

int ChannelCount(const AVCodecParameters* par) {
#if IJKPP_FFMPEG_HAS_CHANNEL_LAYOUT
  return par->ch_layout.nb_channels;
#else
  return par->channels;
#endif
}

uint64_t ChannelLayoutMask(const AVCodecParameters* par) {
#if IJKPP_FFMPEG_HAS_CHANNEL_LAYOUT
  if (par->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) {
    return par->ch_layout.u.mask;
  }
  return 0;
#else
  return par->channel_layout;
#endif
}

SwrPtr MakeSwrContext(AVSampleFormat out_format, uint64_t out_layout,
                      int out_rate, AVSampleFormat in_format, uint64_t in_layout,
                      int in_rate) {
#if IJKPP_FFMPEG_HAS_CHANNEL_LAYOUT
  SwrContext* ctx = nullptr;
  AVChannelLayout ol{};
  AVChannelLayout il{};
  if (out_layout) av_channel_layout_from_mask(&ol, out_layout);
  else            av_channel_layout_default(&ol, 2);
  if (in_layout)  av_channel_layout_from_mask(&il, in_layout);
  else            av_channel_layout_default(&il, 2);
  const int ret = swr_alloc_set_opts2(&ctx, &ol, out_format, out_rate, &il,
                                      in_format, in_rate, 0, nullptr);
  av_channel_layout_uninit(&ol);
  av_channel_layout_uninit(&il);
  if (ret < 0 || !ctx) {
    return SwrPtr();
  }
  return SwrPtr(ctx);
#else
  return SwrPtr(swr_alloc_set_opts(nullptr, static_cast<int64_t>(out_layout),
                                   out_format, out_rate,
                                   static_cast<int64_t>(in_layout), in_format,
                                   in_rate, 0, nullptr));
#endif
}

base::TimeDelta ToTimeDelta(int64_t ts, AVRational time_base) {
  if (ts == AV_NOPTS_VALUE) {
    return media::kNoTimestamp;
  }
  return base::Microseconds(
      av_rescale_q(ts, time_base, AVRational{1, AV_TIME_BASE}));
}

int64_t FromTimeDelta(base::TimeDelta t, AVRational time_base) {
  if (media::IsNoTimestamp(t)) {
    return AV_NOPTS_VALUE;
  }
  return av_rescale_q(t.InMicroseconds(), AVRational{1, AV_TIME_BASE}, time_base);
}

std::string AvErrorString(int av_error) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(av_error, buf, sizeof(buf));
  return std::string(buf);
}

namespace {

// Maps the AVERROR values that carry real diagnostic meaning. Everything else
// falls through to kDecodeFailed with av_strerror's text, so an unmapped code is
// still reported readably rather than as "unknown error".
ErrorCode ClassifyAvError(int av_error) {
  switch (av_error) {
    case AVERROR_EOF:                 return ErrorCode::kSourceEos;
    case AVERROR(ENOENT):             return ErrorCode::kSourceNotFound;
    case AVERROR(EACCES):
    case AVERROR(EPERM):              return ErrorCode::kSourcePermissionDenied;
    case AVERROR(EAGAIN):             return ErrorCode::kTimeout;
    case AVERROR(ENOMEM):             return ErrorCode::kOutOfMemory;
    case AVERROR(EIO):                return ErrorCode::kSourceReadFailed;
    case AVERROR(ETIMEDOUT):          return ErrorCode::kNetworkTimeout;
    case AVERROR(ECONNREFUSED):
    case AVERROR(EHOSTUNREACH):
    case AVERROR(ENETUNREACH):        return ErrorCode::kNetworkUnreachable;
    case AVERROR_PROTOCOL_NOT_FOUND:
    case AVERROR(EINVAL):             return ErrorCode::kSourceUnsupported;
    case AVERROR_INVALIDDATA:         return ErrorCode::kDecodeFailed;
    case AVERROR(ESPIPE):             return ErrorCode::kSourceSeekFailed;
    case AVERROR_BUG:
    case AVERROR_BUG2:
    case AVERROR_PATCHWELCOME:        return ErrorCode::kNotImplemented;
    case AVERROR_EXIT:                return ErrorCode::kAborted;
    case AVERROR_DECODER_NOT_FOUND:
    case AVERROR_STREAM_NOT_FOUND:    return ErrorCode::kStreamNotFound;
    default:                          return ErrorCode::kDecodeFailed;
  }
}

std::string SummaryFor(ErrorCode code) {
  switch (code) {
    case ErrorCode::kSourceEos:               return "end of stream";
    case ErrorCode::kSourceNotFound:          return "media source not found";
    case ErrorCode::kSourcePermissionDenied:  return "permission denied opening the media source";
    case ErrorCode::kSourceReadFailed:        return "I/O error while reading the media source";
    case ErrorCode::kSourceSeekFailed:        return "seek failed on this source";
    case ErrorCode::kSourceUnsupported:       return "no demuxer or protocol can handle this source";
    case ErrorCode::kNetworkTimeout:          return "network read timed out";
    case ErrorCode::kNetworkUnreachable:      return "network unreachable or connection refused";
    case ErrorCode::kOutOfMemory:             return "FFmpeg ran out of memory";
    case ErrorCode::kTimeout:                 return "operation would block; retry";
    case ErrorCode::kAborted:                 return "operation aborted";
    case ErrorCode::kStreamNotFound:          return "requested stream or decoder not found";
    case ErrorCode::kNotImplemented:          return "FFmpeg reported an unimplemented path";
    default:                                  return "decoding or demuxing failed";
  }
}

}  // namespace

MediaError ToMediaError(int av_error, std::string_view context) {
  return ToMediaError(av_error, context, {}, {});
}

MediaError ToMediaError(int av_error, std::string_view context,
                        std::string_view detail, std::string_view suggestion) {
  if (av_error >= 0) {
    return MediaError::Ok();
  }
  const ErrorCode code = ClassifyAvError(av_error);
  std::string det = std::string("native = ") + std::to_string(av_error) +
                    " (" + AvErrorString(av_error) + ")";
  if (!detail.empty()) {
    det = std::string(detail) + "\n           " + det;
  }
  std::string sug(suggestion);
  if (sug.empty()) {
    switch (code) {
      case ErrorCode::kSourceNotFound:
        sug = "verify the path or URL is reachable; for local files check that "
              "the file exists and is readable by this process";
        break;
      case ErrorCode::kSourceUnsupported:
        sug = "set config.demux.forced_format to name the container explicitly "
              "(e.g. \"h264\", \"mpegts\"), or raise config.demux.probe_size";
        break;
      case ErrorCode::kNetworkTimeout:
        sug = "raise config.demux.timeout, or increase "
              "config.net.reconnect_max_retries";
        break;
      default:
        sug = "attach Player::DumpDiagnostics() to the bug report";
        break;
    }
  }
  return MediaError(code, SummaryFor(code), std::move(det),
                    std::move(sug), av_error, std::string(context));
}

media::DecoderStatus ToDecoderStatus(int av_error, std::string_view context) {
  using Codes = media::DecoderStatus::Codes;
  if (av_error >= 0) {
    return media::DecoderStatus();
  }
  Codes code = Codes::kDecodeError;
  switch (av_error) {
    case AVERROR(EAGAIN):     code = Codes::kDecodeError; break;
    case AVERROR_EOF:         code = Codes::kDecodeError; break;
    case AVERROR_DECODER_NOT_FOUND: code = Codes::kUnsupportedCodec; break;
    case AVERROR_INVALIDDATA: code = Codes::kDecodeError; break;
    case AVERROR(ENOMEM):     code = Codes::kUnknownError; break;
    case AVERROR_EXIT:        code = Codes::kDecodingAborted; break;
    default:                  code = Codes::kUnknownError; break;
  }
  return media::DecoderStatus(
      code, std::string(context) + ": " + AvErrorString(av_error));
}

DictPtr ToAvDict(const std::map<std::string, std::string>& options) {
  // av_dict_set() takes an AVDictionary** and may allocate on the first call,
  // so the unique_ptr has to be released and re-adopted each iteration.
  DictPtr dict;
  for (const auto& [key, value] : options) {
    AVDictionary* raw = dict.release();
    av_dict_set(&raw, key.c_str(), value.c_str(), 0);
    dict.reset(raw);
  }
  return dict;
}

std::map<std::string, std::string> FromAvDict(const AVDictionary* dict) {
  std::map<std::string, std::string> out;
  const AVDictionaryEntry* entry = nullptr;
#if IJKPP_FFMPEG_HAS_DICT_ITERATE
  while ((entry = av_dict_iterate(dict, entry)) != nullptr) {
    out[entry->key] = entry->value ? entry->value : "";
  }
#else
  // FFmpeg < 6.0: av_dict_get with an empty key and IGNORE_SUFFIX walks the
  // whole dictionary, returning the entry after |entry| each time.
  while ((entry = av_dict_get(dict, "", entry, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
    out[entry->key] = entry->value ? entry->value : "";
  }
#endif
  return out;
}

std::vector<std::string> UnconsumedOptions(const AVDictionary* dict) {
  std::vector<std::string> out;
  for (const auto& [key, value] : FromAvDict(dict)) {
    out.push_back(key);
  }
  return out;
}

int64_t PacketDuration(const AVPacket* packet) {
#if IJKPP_FFMPEG_PACKET_INT64
  return packet->duration;
#else
  return static_cast<int64_t>(packet->duration);
#endif
}

}  // namespace ijkpp::platform::ffmpeg
