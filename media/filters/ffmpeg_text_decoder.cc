// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/ffmpeg_text_decoder.h"

#include <utility>

#include "base/logging.h"
#include "media/base/timed_text.h"
#include "platform/ffmpeg/av_includes.h"
#include "platform/ffmpeg/av_packet_storage.h"
#include "platform/ffmpeg/compat.h"

namespace avbase::media {
namespace {

namespace ff = ::avbase::platform::ffmpeg;

// Flattens one AVSubtitle rect set into a cue. Plain-text rects join with
// newlines; ASS markup goes verbatim into |ass| so a host that lays out can
// use it while the default path still has readable text.
TimedTextCue MakeCue(const AVSubtitle& sub, base::TimeDelta packet_pts,
                     base::TimeDelta buffer_duration) {
  TimedTextCue cue;
  std::string text;
  std::string ass;
  for (unsigned i = 0; i < sub.num_rects; ++i) {
    const AVSubtitleRect* rect = sub.rects[i];
    if (!rect) {
      continue;
    }
    if (rect->type == SUBTITLE_ASS && rect->ass) {
      if (!ass.empty()) {
        ass += "\n";
      }
      ass += rect->ass;
      // ASS dialogue lines carry their own formatting prefix and markup.
      // For the plain-text view: strip the {...} override blocks, cut the
      // dialogue field header (ffmpeg's rect->ass is
      // "ReadOrder,Layer,Style,Name,ML,MR,MV,Effect,Text" -- the text starts
      // after the 8th comma), and turn ASS line-break escapes into real
      // newlines so the default consumer still sees the words.
      std::string line = rect->ass;
      std::string plain;
      bool in_braces = false;
      for (const char ch : line) {
        if (ch == '{') {
          in_braces = true;
        } else if (ch == '}') {
          in_braces = false;
        } else if (!in_braces) {
          plain += ch;
        }
      }
      int commas = 0;
      size_t text_start = 0;
      for (size_t i = 0; i < plain.size() && commas < 8; ++i) {
        if (plain[i] == ',') {
          ++commas;
          text_start = i + 1;
        }
      }
      if (commas == 8) {
        plain = plain.substr(text_start);
      }
      for (size_t pos = plain.find("\\N"); pos != std::string::npos;
           pos = plain.find("\\N")) {
        plain.replace(pos, 2, "\n");
      }
      for (size_t pos = plain.find("\\n"); pos != std::string::npos;
           pos = plain.find("\\n")) {
        plain.replace(pos, 2, "\n");
      }
      for (size_t pos = plain.find("\\h"); pos != std::string::npos;
           pos = plain.find("\\h")) {
        plain.replace(pos, 2, " ");
      }
      if (!text.empty()) {
        text += "\n";
      }
      text += plain;
    } else if (rect->type == SUBTITLE_TEXT && rect->text) {
      if (!text.empty()) {
        text += "\n";
      }
      text += rect->text;
    }
  }
  cue.pts = packet_pts + base::Milliseconds(sub.start_display_time);
  cue.duration =
      base::Milliseconds(sub.end_display_time - sub.start_display_time);
  if (cue.duration <= base::TimeDelta()) {
    // Some text decoders (subrip among them) leave the relative display
    // window at zero and let the container carry the duration: the demuxer
    // stamped it on the buffer from the packet/mkv BlockDuration.
    cue.duration = buffer_duration;
  }
  cue.text = std::move(text);
  cue.ass = std::move(ass);
  return cue;
}

}  // namespace

struct FFmpegTextDecoder::Context {
  ff::CodecCtxPtr codec_ctx;
};

FFmpegTextDecoder::FFmpegTextDecoder() = default;

FFmpegTextDecoder::~FFmpegTextDecoder() = default;

Status FFmpegTextDecoder::Initialize(const TextDecoderConfig& config) {
  if (!config.IsValidConfig()) {
    return base::unexpected(MediaError(
        ErrorCode::kInvalidArgument, "text decoder config has no codec",
        "TextDecoderConfig::codec_name is empty",
        "the demuxer must fill text_decoder_config() for subtitle streams"));
  }
  const AVCodec* codec =
      avcodec_find_decoder_by_name(config.codec_name.c_str());
  if (!codec || codec->type != AVMEDIA_TYPE_SUBTITLE) {
    return base::unexpected(MediaError(
        ErrorCode::kNotImplemented,
        "no subtitle decoder named \"" + config.codec_name + "\"",
        "this FFmpeg build does not enable that text codec",
        "check the subtitle codec list (ffmpeg -decoders | grep subtitle) "
        "and re-run tools/setup_ffmpeg.sh for a full build"));
  }
  ff::CodecCtxPtr codec_ctx(avcodec_alloc_context3(codec));
  if (!codec_ctx) {
    return base::unexpected(
        MediaError(ErrorCode::kOutOfMemory, "avcodec_alloc_context3 failed",
                   "text decoder context allocation returned null",
                   "retry; if it persists this is an OOM report"));
  }
  if (!config.extra_data.empty()) {
    codec_ctx->extradata = static_cast<uint8_t*>(
        av_mallocz(config.extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!codec_ctx->extradata) {
      return base::unexpected(MediaError(
          ErrorCode::kOutOfMemory, "extradata allocation failed",
          "text decoder extradata allocation returned null", "retry"));
    }
    memcpy(codec_ctx->extradata, config.extra_data.data(),
           config.extra_data.size());
    codec_ctx->extradata_size = static_cast<int>(config.extra_data.size());
  }
  const int ret = avcodec_open2(codec_ctx.get(), codec, nullptr);
  if (ret < 0) {
    return base::unexpected(MediaError(
        ErrorCode::kDecoderOpenFailed,
        "cannot open the subtitle decoder for \"" + config.codec_name + "\"",
        "avcodec_open2: " + ff::AvErrorString(ret),
        "check that the stream's subtitle codec matches the container"));
  }
  ctx_ = std::make_unique<Context>();
  ctx_->codec_ctx = std::move(codec_ctx);
  config_ = config;
  return Status();
}

Status FFmpegTextDecoder::Decode(const DecoderBuffer& buffer,
                                 std::vector<TimedTextCue>* cues) {
  if (!ctx_ || !ctx_->codec_ctx) {
    return base::unexpected(
        MediaError(ErrorCode::kInvalidState, "text decoder is not initialized",
                   "Decode() ran before a successful Initialize()",
                   "this is an avbase bug; report the event log"));
  }
  auto* storage = buffer.storage_as<ff::AvPacketStorage>();
  if (!storage || !storage->raw()) {
    return base::unexpected(MediaError(
        ErrorCode::kDecodeFailed, "text buffer storage is not an AVPacket",
        "a custom Demuxer must produce AvPacketStorage for FFmpegTextDecoder",
        "wrap packets the same way the FFmpeg demuxer does"));
  }

  AVSubtitle sub;
  memset(&sub, 0, sizeof(sub));
  int got_subtitle = 0;
  AVPacket* packet = const_cast<AVPacket*>(storage->raw());
  const int ret = avcodec_decode_subtitle2(ctx_->codec_ctx.get(), &sub,
                                           &got_subtitle, packet);
  if (ret < 0) {
    avsubtitle_free(&sub);
    return base::unexpected(MediaError(
        ErrorCode::kDecodeFailed, "the subtitle packet could not be decoded",
        "avcodec_decode_subtitle2: " + ff::AvErrorString(ret),
        "skip-on-error is the text leg's policy: one bad cue never "
        "stops playback, but a stream of them means the track does "
        "not match its codec description"));
  }
  if (got_subtitle) {
    // The demuxer already stamped buffer->timestamp() with the STREAM time
    // base -- the authoritative container timing. packet->time_base is not
    // uniformly maintained for subtitle streams (MKV reports 1/1 here while
    // the pts are milliseconds), so the packet field is not trusted.
    // AVSubtitle's display offsets are milliseconds relative to the packet
    // pts.
    const base::TimeDelta pts = buffer.timestamp();
    cues->push_back(MakeCue(sub, pts, buffer.duration()));
  }
  avsubtitle_free(&sub);
  return Status();
}

std::unique_ptr<TextDecoder>
FFmpegTextDecoderFactory::CreateTextDecoder(const TextDecoderConfig& config) {
  auto decoder = std::make_unique<FFmpegTextDecoder>();
  if (!decoder->Initialize(config)) {
    LOG(WARNING) << "FFmpegTextDecoderFactory: cannot initialize a text "
                 << "decoder for codec \"" << config.codec_name << "\"";
    return nullptr;
  }
  return decoder;
}

}  // namespace avbase::media
