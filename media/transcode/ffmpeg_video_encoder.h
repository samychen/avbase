// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_TRANSCODE_VIDEO_ENCODER_H_
#define AVBASE_MEDIA_TRANSCODE_VIDEO_ENCODER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "media/base/encoded_packet.h"
#include "media/base/media_error.h"
#include "media/base/video_frame.h"
#include "media/ffmpeg/av_includes.h"
#include "media/media_export.h"

namespace avbase::media {

// Video encoder over libavcodec — the video half of the E1 encoder layer.
// |codec_name| selects the encoder: "mjpeg" works in the base pinned build,
// "libx264" when the x264 dependency layer is on (tools/setup_ffmpeg.sh).
// Input: I420 VideoFrames (converted internally to the encoder's format).
class AVBASE_MEDIA_EXPORT FFmpegVideoEncoder {
 public:
  struct Params {
    std::string codec_name = "libx264";
    int width = 0;
    int height = 0;
    int bit_rate = 0;    // bps; 0 = default/CRF-driven.
    int crf = -1;        // libx264 quality; -1 = unused.
    int gop = 0;         // 0 = encoder default.
    std::string preset;  // libx264 preset (ultrafast..veryslow).
    // Frame rate as a rational; defaults to 30/1. ffmpeg.c sets this from
    // -r / the input stream's avg_frame_rate. The encoder's time_base is
    // derived as 1/denominator so packet pts land in 1/fps units.
    int fps_num = 30;
    int fps_den = 1;
  };

  FFmpegVideoEncoder();
  FFmpegVideoEncoder(const FFmpegVideoEncoder&) = delete;
  FFmpegVideoEncoder& operator=(const FFmpegVideoEncoder&) = delete;
  ~FFmpegVideoEncoder();

  bool Initialize(const Params& params);

  // Codec parameters for the muxer (extradata included); empty before
  // Initialize.
  const AVCodecParameters* codec_parameters() const;

  // The encoder's timebase (1/fps_denominator). Muxer callers use this to
  // know what units packet pts/dts are in.
  AVRational time_base() const;

  // Encodes one frame; packets accumulate in |out| in presentation order.
  // Each packet carries pts/dts/flags/side_data from the encoder.
  bool Encode(base::scoped_refptr<VideoFrame> in,
              std::vector<EncodedPacket>* out);

  // Drains the encoder delay after the last frame.
  bool Flush(std::vector<EncodedPacket>* out);

 private:
  struct Context;
  std::unique_ptr<Context> ctx_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_TRANSCODE_VIDEO_ENCODER_H_
