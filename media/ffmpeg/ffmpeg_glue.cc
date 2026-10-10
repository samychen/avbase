// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_glue.h"

#include <atomic>
#include <mutex>

#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/log_bridge.h"

namespace avbase::media::ffmpeg {
namespace {

std::once_flag g_once;
std::atomic<bool> g_initialized{false};

void DoInitialize() {
#if AVBASE_FFMPEG_NEEDS_REGISTER_ALL
  av_register_all();
#endif
  media::ffmpeg::InstallLogBridge();
  g_initialized.store(true, std::memory_order_release);
}

}  // namespace

void InitializeFFmpeg() {
  std::call_once(g_once, &DoInitialize);
}

bool IsFFmpegInitialized() {
  return g_initialized.load(std::memory_order_acquire);
}

// The one definition. It used to be a wrapper delegating to namespace
// avbase::platform::ffmpeg, where log_bridge.cc held the real one; the two
// halves of the FFmpeg code lived in two namespaces and only one of them was
// allowed to know libav*. Now that all of it is media::ffmpeg, the wrapper
// became a duplicate symbol that called itself -- so the body moved here and
// log_bridge.cc lost its copy.
std::string GetFFmpegVersionString() {
  return std::string(LIBAVFORMAT_IDENT) + " / " + LIBAVCODEC_IDENT;
}

std::string GetFFmpegConfigurationSummary() {
  // Walk the registered components rather than parsing --enable-* flags, so the
  // summary reflects what this binary can actually do.
  std::string out;
  // av_*_iterate() takes an opaque void** cookie, not the previous entry.
  int demuxers = 0;
  void* demuxer_cookie = nullptr;
  while (av_demuxer_iterate(&demuxer_cookie) != nullptr) {
    ++demuxers;
  }
  int decoders = 0;
  void* codec_cookie = nullptr;
  while (const AVCodec* c = av_codec_iterate(&codec_cookie)) {
    if (av_codec_is_decoder(c)) {
      ++decoders;
    }
  }
  out += "demuxers=" + std::to_string(demuxers);
  out += " decoders=" + std::to_string(decoders);
  out += std::string(" protocols=") +
         (av_find_input_format("hls") ? "hls " : "") +
         (av_find_input_format("matroska") ? "matroska " : "") +
         (av_find_input_format("mov") ? "mov " : "");
  return out;
}

}  // namespace avbase::media::ffmpeg
