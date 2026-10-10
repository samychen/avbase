// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_ENCODE_MUXER_H_
#define AVBASE_MEDIA_FFMPEG_ENCODE_MUXER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "media/base/media_error.h"
#include "media/filters/encoded_packet.h"
#include "media/media_export.h"

struct AVRational;

namespace avbase::media {

// The container write end for encoded streams — RemuxContainer's skeleton
// generalized so the packets come from encoders instead of a demuxer. This
// is the E2 slab of the transcode port: transcode.cc's Muxer role, split
// out of the encoder objects (E1) so each stays testable.
//
// Stream geometry: audio streams tick in 1/sample_rate units, video streams
// in 1/encoder_time_base. Callers pass EncodedPacket (pts/dts/flags/side_data
// from the encoder); the muxer rescales into the container's timebase on
// write and passes flags through verbatim instead of guessing keyframes.
class AVBASE_MEDIA_EXPORT FFmpegEncodeMuxer {
 public:
  struct AudioStreamParams {
    int sample_rate = 48000;
    int channels = 2;
    std::string codec_name = "aac";
    std::vector<uint8_t> extradata;  // AudioSpecificConfig etc.
  };
  struct VideoStreamParams {
    int width = 0;
    int height = 0;
    std::string codec_name = "libx264";  // or "mjpeg" in the base build.
    std::vector<uint8_t> extradata;
    int bit_rate = 0;  // 0 = container/codec default.
    // The encoder's timebase; packet pts/dts arrive in these units and are
    // rescaled to the container's stream timebase on write. Defaults to
    // 1/90000 (video). Set from FFmpegVideoEncoder::time_base() when the
    // caller wires a real encoder.
    int time_base_num = 1;
    int time_base_den = 90000;
  };

  FFmpegEncodeMuxer();
  FFmpegEncodeMuxer(const FFmpegEncodeMuxer&) = delete;
  FFmpegEncodeMuxer& operator=(const FFmpegEncodeMuxer&) = delete;
  ~FFmpegEncodeMuxer();

  // Opens the output container; the muxer follows the file extension
  // (mp4/matroska/adts are enabled in the pinned build). |options| is a
  // key-value list passed to avformat_write_header (e.g. "movflags"→
  // "+faststart"); may be empty.
  bool
  Open(const std::string& path,
       const std::vector<std::pair<std::string, std::string>>& options = {});

  // Adds tracks before the first WritePacket. Returns the stream index or
  // -1 with the reason in the log.
  int AddAudioStream(const AudioStreamParams& params);
  int AddVideoStream(const VideoStreamParams& params);

  // Writes one encoded packet. The packet's pts/dts are in the stream's
  // own timebase; flags (keyframe etc.) and side_data are passed through
  // from the encoder, not guessed.
  bool WritePacket(int stream_index, const EncodedPacket& packet);

  // Writes the trailer and closes. The object is reusable after Open().
  bool Finish();

 private:
  struct Context;
  std::unique_ptr<Context> ctx_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FFMPEG_ENCODE_MUXER_H_
