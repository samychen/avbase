// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_TRANSCODE_AUDIO_ENCODER_H_
#define AVBASE_MEDIA_TRANSCODE_AUDIO_ENCODER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "media/base/audio_buffer.h"
#include "media/base/media_error.h"
#include "media/filters/encoded_packet.h"
#include "media/media_export.h"

namespace avbase::media {

// libavcodec-backed audio encoder — the first slab of the transcode encoder
// layer (E1). The transcode.cc equivalent embedded its encoders inside
// Muxer::OpenAudio; here the encoder is a standalone stage so EncodeMuxer
// (E2) can take encoder output the same way RemuxContainer takes demuxer
// packets.
//
// Generic: |codec_name| selects the encoder through
// avcodec_find_encoder_by_name ("aac" works in the base pinned build,
// "libmp3lame" / "libopus" / "flac" / "ac3" wherever those encoders are built
// into FFmpeg). This is the audio mirror of FFmpegVideoEncoder, which is
// already codec-name driven.
//
// Input contract: planar float AudioBuffers (the decode path's output),
// single rate/channels fixed at Initialize. Output: raw packets plus the
// codec extradata (an ADTS writer or a muxer adds the framing for codecs
// that need it).
class AVBASE_MEDIA_EXPORT FFmpegAudioEncoder {
 public:
  struct Params {
    // Encoder name as understood by avcodec_find_encoder_by_name. Defaults to
    // "aac" so existing callers keep producing AAC; anything else (e.g.
    // "libopus", "libmp3lame") selects that encoder when the build has it.
    std::string codec_name = "aac";
    int sample_rate = 48000;
    int channels = 2;
    int bit_rate = 128000;  // bps; 0 = encoder default.
  };

  FFmpegAudioEncoder();
  FFmpegAudioEncoder(const FFmpegAudioEncoder&) = delete;
  FFmpegAudioEncoder& operator=(const FFmpegAudioEncoder&) = delete;
  ~FFmpegAudioEncoder();

  // False with the reason in the log (encoder absent from the FFmpeg build,
  // unsupported channel count, or the codec refuses the chosen geometry).
  bool Initialize(const Params& params);

  // Codec extradata (AudioSpecificConfig for AAC, OpusHead for Opus, ...);
  // empty before Initialize or for codecs that carry none (e.g. MP3).
  const std::vector<uint8_t>& extradata() const { return ctx_extradata_; }

  // Samples per codec frame (1024 for AAC-LC, 1152 for MP3, 960 for Opus,
  // 0 for variable-frame codecs like FLAC); 0 before Initialize. Callers
  // that resample must hand the encoder whole frames: libavcodec rejects an
  // arbitrary count with EINVAL unless it is the last frame. A 0 here means
  // the encoder accepts whatever the caller hands it (no FIFO framing).
  int frame_size() const;

  // Encodes one buffer; encoded packets accumulate in |out| (in presentation
  // order). Each packet carries pts/dts/flags/duration in 1/sample_rate units.
  // Buffers must match the initialized geometry. Returns false on encoder
  // failure.
  bool Encode(base::scoped_refptr<AudioBuffer> in,
              std::vector<EncodedPacket>* out);

  // Flushing the encoder delay; call once after the last Encode. EOS in
  // (null) drains, so the last frames are not lost.
  bool Flush(std::vector<EncodedPacket>* out);

  // Input media time of the next packet, for muxers that stamp.
  int64_t next_pts_samples() const { return ctx_next_pts_samples_; }

 private:
  struct Context;
  std::unique_ptr<Context> ctx_;
  std::vector<uint8_t> ctx_extradata_;
  int64_t ctx_next_pts_samples_ = 0;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_TRANSCODE_AUDIO_ENCODER_H_
