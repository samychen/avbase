// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_remuxer.h"

#include <memory>
#include <string>

#include "base/logging.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace avbase::media {
namespace {

namespace ff = ::avbase::media::ffmpeg;

Status Fail(const std::string& summary, const std::string& detail,
            const std::string& suggestion) {
  return base::unexpected(
      MediaError(ErrorCode::kSourceReadFailed, summary, detail, suggestion));
}

}  // namespace

Status RemuxContainer(const std::string& src_path,
                      const std::string& dst_path) {
  ff::FormatCtxPtr in(nullptr);
  AVFormatContext* raw_in = nullptr;
  if (avformat_open_input(&raw_in, src_path.c_str(), nullptr, nullptr) < 0) {
    return Fail("cannot open " + src_path, "avformat_open_input failed",
                "check the source file exists and is a supported container");
  }
  in.reset(raw_in);
  if (avformat_find_stream_info(in.get(), nullptr) < 0) {
    return Fail("cannot probe " + src_path, "avformat_find_stream_info failed",
                {});
  }

  // The output container follows the extension; avformat picks the muxer.
  AVFormatContext* raw_out = nullptr;
  if (avformat_alloc_output_context2(&raw_out, nullptr, nullptr,
                                     dst_path.c_str()) < 0 ||
      !raw_out) {
    return Fail(
        "cannot create " + dst_path,
        "avformat_alloc_output_context2 failed (unsupported extension?)",
        "mp4, matroska (mkv) and adts (aac) muxers are enabled in "
        "tools/setup_ffmpeg.sh");
  }
  ff::FormatCtxPtr out(raw_out);

  for (unsigned i = 0; i < in->nb_streams; ++i) {
    AVStream* out_stream = avformat_new_stream(out.get(), nullptr);
    if (!out_stream) {
      return Fail("out of streams while creating " + dst_path,
                  "avformat_new_stream failed", {});
    }
    if (avcodec_parameters_copy(out_stream->codecpar,
                                in->streams[i]->codecpar) < 0) {
      return Fail("stream parameter copy failed for " + dst_path, {}, {});
    }
    out_stream->codecpar->codec_tag = 0;
  }

  if (!(out->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&out->pb, dst_path.c_str(), AVIO_FLAG_WRITE) < 0) {
      return Fail("cannot open " + dst_path + " for writing",
                  "avio_open failed", "check the directory is writable");
    }
  }
  if (avformat_write_header(out.get(), nullptr) < 0) {
    return Fail("write header failed for " + dst_path,
                "avformat_write_header failed",
                "the destination container may not support the source "
                "codecs");
  }

  ff::PacketPtr packet(av_packet_alloc());
  while (av_read_frame(in.get(), packet.get()) >= 0) {
    AVStream* in_stream = in->streams[packet->stream_index];
    AVStream* out_stream = out->streams[packet->stream_index];
    av_packet_rescale_ts(packet.get(), in_stream->time_base,
                         out_stream->time_base);
    packet->pos = -1;
    if (av_interleaved_write_frame(out.get(), packet.get()) < 0) {
      return Fail("packet write failed for " + dst_path,
                  "av_interleaved_write_frame failed at pts " +
                      std::to_string(packet->pts),
                  "the destination container may not support this codec");
    }
    av_packet_unref(packet.get());
  }

  if (av_write_trailer(out.get()) < 0) {
    return Fail("write trailer failed for " + dst_path, {}, {});
  }
  if (!(out->oformat->flags & AVFMT_NOFILE)) {
    avio_closep(&out->pb);
  }
  LOG(INFO) << "remuxed " << src_path << " -> " << dst_path;
  return Status();
}

}  // namespace avbase::media
