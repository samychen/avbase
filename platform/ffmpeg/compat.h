// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Every FFmpeg version difference in avbase is absorbed here. Code outside
// platform/ffmpeg/ uses only these wrappers and never sees an `#if LIBAV*`.

#ifndef AVBASE_PLATFORM_FFMPEG_COMPAT_H_
#define AVBASE_PLATFORM_FFMPEG_COMPAT_H_

#include <map>
#include <memory>
#include <vector>
#include <string>
#include <string_view>

#include "base/time/time.h"
#include "media/base/decoder_status.h"
#include "player/public/error.h"
#include "platform/ffmpeg/av_includes.h"

namespace avbase::platform::ffmpeg {

// ---- RAII wrappers ---------------------------------------------------------
// These replace the 50+ `goto fail` ladders in ijkplayer's ff_ffplay.c: with a
// unique_ptr per FFmpeg handle, an early return is correct by construction.
struct FormatCtxDeleter { void operator()(AVFormatContext* p) const { if (p) avformat_close_input(&p); } };
struct CodecCtxDeleter  { void operator()(AVCodecContext* p)  const { if (p) avcodec_free_context(&p); } };
struct FrameDeleter     { void operator()(AVFrame* p)         const { if (p) av_frame_free(&p); } };
struct PacketDeleter    { void operator()(AVPacket* p)        const { if (p) av_packet_free(&p); } };
struct DictDeleter      { void operator()(AVDictionary* p)    const { if (p) av_dict_free(&p); } };
struct BufferDeleter    { void operator()(AVBufferRef* p)     const { if (p) av_buffer_unref(&p); } };
struct SwrDeleter       { void operator()(SwrContext* p)      const { if (p) swr_free(&p); } };
struct SwsDeleter       { void operator()(SwsContext* p)      const { if (p) sws_freeContext(p); } };
struct IoCtxDeleter     { void operator()(AVIOContext* p)     const { if (p) avio_context_free(&p); } };

using FormatCtxPtr = std::unique_ptr<AVFormatContext, FormatCtxDeleter>;
using CodecCtxPtr  = std::unique_ptr<AVCodecContext,  CodecCtxDeleter>;
using FramePtr     = std::unique_ptr<AVFrame,     FrameDeleter>;
using PacketPtr    = std::unique_ptr<AVPacket,    PacketDeleter>;
using DictPtr      = std::unique_ptr<AVDictionary, DictDeleter>;
using BufferPtr    = std::unique_ptr<AVBufferRef,  BufferDeleter>;
using SwrPtr       = std::unique_ptr<SwrContext,   SwrDeleter>;
using SwsPtr       = std::unique_ptr<SwsContext,   SwsDeleter>;
using IoCtxPtr     = std::unique_ptr<AVIOContext,  IoCtxDeleter>;

// ---- Channel layout (the AVChannelLayout boundary) --------------------------
int ChannelCount(const AVCodecContext* ctx);
uint64_t ChannelLayoutMask(const AVCodecContext* ctx);
void SetChannelLayout(AVCodecContext* ctx, uint64_t mask, int channels);
// Same, from an AVStream's codecpar (used while probing).
int ChannelCount(const AVCodecParameters* par);
uint64_t ChannelLayoutMask(const AVCodecParameters* par);

// ---- swresample allocation (swr_alloc_set_opts -> swr_alloc_set_opts2) ------
SwrPtr MakeSwrContext(AVSampleFormat out_format, uint64_t out_layout, int out_rate,
                      AVSampleFormat in_format,  uint64_t in_layout,  int in_rate);

// ---- Timestamps -------------------------------------------------------------
// AV_NOPTS_VALUE becomes media::kNoTimestamp; no magic INT64_MIN ever escapes
// into avbase's own code.
base::TimeDelta ToTimeDelta(int64_t ts, AVRational time_base);
int64_t FromTimeDelta(base::TimeDelta t, AVRational time_base);

// ---- Errors -----------------------------------------------------------------
// Translates an AVERROR into avbase's three-part MediaError (docs/10 §4), using
// av_strerror for the human-readable text.
MediaError ToMediaError(int av_error, std::string_view context);
MediaError ToMediaError(int av_error, std::string_view context,
                        std::string_view detail, std::string_view suggestion);
media::DecoderStatus ToDecoderStatus(int av_error, std::string_view context);

// ---- Dictionaries -----------------------------------------------------------
DictPtr ToAvDict(const std::map<std::string, std::string>& options);
std::map<std::string, std::string> FromAvDict(const AVDictionary* dict);
// Keys FFmpeg did not consume, which is how a typo'd extra_format_options entry
// gets reported instead of being silently ignored.
std::vector<std::string> UnconsumedOptions(const AVDictionary* dict);

// ---- Misc -------------------------------------------------------------------
std::string AvErrorString(int av_error);
int64_t PacketDuration(const AVPacket* packet);

}  // namespace avbase::platform::ffmpeg

#endif  // AVBASE_PLATFORM_FFMPEG_COMPAT_H_
