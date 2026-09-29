// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/ffmpeg_glue.h"

#include <atomic>
#include <mutex>

#include "platform/ffmpeg/av_includes.h"
#include "platform/ffmpeg/log_bridge.h"

namespace ijkpp::media::ffmpeg {
namespace {

std::once_flag g_once;
std::atomic<bool> g_initialized{false};

void DoInitialize() {
#if IJKPP_FFMPEG_NEEDS_REGISTER_ALL
  av_register_all();
#endif
  platform::ffmpeg::InstallLogBridge();
  g_initialized.store(true, std::memory_order_release);
}

}  // namespace

void InitializeFFmpeg() { std::call_once(g_once, &DoInitialize); }

bool IsFFmpegInitialized() {
  return g_initialized.load(std::memory_order_acquire);
}

std::string GetFFmpegVersionString() {
  return platform::ffmpeg::GetFFmpegVersionString();
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

}  // namespace ijkpp::media::ffmpeg
