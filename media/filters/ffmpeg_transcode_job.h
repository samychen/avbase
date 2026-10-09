// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_FFMPEG_TRANSCODE_JOB_H_
#define AVBASE_MEDIA_FILTERS_FFMPEG_TRANSCODE_JOB_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "media/base/decoder_config.h"  // VideoCodec
#include "media/base/media_error.h"
#include "media/filters/decoder_selector.h"  // HwCodecFlag, HwCodecMask
#include "media/filters/encoded_packet.h"
#include "media/media_export.h"
#include "platform/ffmpeg/video_encoder_factory.h"

namespace avbase::media {

// Offline transcode job: the E3 slab. Mirrors ffmpeg.c's transcode() loop and
// transcode.cc's TranscodeTask — a synchronous, run-to-completion pipeline
// that does NOT pace against any clock. The input is demuxed, decoded (when
// re-encoding), encoded, and muxed into the output container in one pass.
//
// Trim: |start_time| and |duration| implement -ss/-t. When |start_time| > 0,
// the input is seeked before the loop starts (the demuxer's keyframe seek,
// not frame-accurate). |duration| caps the output; packets past start+dur
// are dropped. Either may be zero (no trim).
//
// Progress: |progress_cb| is called after each encoded packet with a 0..100
// ratio based on (processed_time / total_duration). May be null.
//
// Stream selection: SetAudioCodec/SetVideoCodec choose the output encoder.
// Leaving either unset drops that stream (ffmpeg.c's -an / -vn).
//
// Copy mode: SetAudioCodec("copy") / SetVideoCodec("copy") enables stream
// copy (RemuxContainer's path, inlined so trim still works). No decode or
// encode happens for copy streams — packets go straight to the muxer.
struct AVBASE_MEDIA_EXPORT TranscodeParams {
  // Output path; the container follows the extension (mp4/mkv/adts...).
  std::string output_path;

  // Trim window. start_time = 0 means from the beginning. duration = 0
  // means to end of file.
  double start_time_seconds = 0.0;
  double duration_seconds = 0.0;

  // Container options passed to avformat_write_header (e.g. "movflags" →
  // "+faststart").
  std::vector<std::pair<std::string, std::string>> muxer_options;

  // Output codec specs. An empty |audio_codec| or |video_codec| drops the
  // stream. "copy" means stream copy (no re-encode).
  struct AudioCodecSpec {
    std::string codec = "aac";  // "copy" or encoder name.
    int bit_rate = 128000;
    int sample_rate = 0;  // 0 = same as input.
    int channels = 0;     // 0 = same as input.
  };
  struct VideoCodecSpec {
    std::string codec = "libx264";  // "copy" or encoder name.
    int bit_rate = 0;               // 0 = CRF-driven when |crf| >= 0.
    int crf = -1;                   // -1 = unused.
    int gop = 0;                    // 0 = default.
    std::string preset;             // libx264 preset.
    // 0,0 = same as input.
    int width = 0;
    int height = 0;
    // 0,0 = same as input frame rate.
    int fps_num = 0;
    int fps_den = 0;

    // --- E5 encoder selection -------------------------------------------
    // When true (the default), the concrete FFmpeg encoder name is resolved
    // through VideoEncoderFactory instead of handing |codec| to FFmpeg
    // verbatim. That is what gives the hardware factories a production
    // caller: before this existed, nothing outside the unit tests ever
    // touched SelectVideoEncoder(), so asking for hardware was silently a
    // no-op no matter how the flags were set.
    bool use_encoder_factory = true;
    // When true, hardware factories outrank the software one, subject to
    // |hw_mask|. Off by default: a machine without working hardware
    // encoding must behave exactly as before, not fail differently.
    bool prefer_hardware = false;
    HwCodecMask hw_mask = static_cast<HwCodecMask>(HwCodecFlag::kAll);
    // Test seam: replaces the platform's factory list. Empty means "query
    // the platform"; mirrors DefaultRendererFactory::Deps.
    std::vector<base::scoped_refptr<platform::ffmpeg::VideoEncoderFactory>>
        factories;
  };
  AudioCodecSpec audio_codec;
  VideoCodecSpec video_codec;
};

// Progress callback: 0..100 integer.
using TranscodeProgressCB = std::function<void(int)>;

// Runs the transcode synchronously. Returns OkStatus on success.
// The caller's thread blocks until the job completes or fails.
AVBASE_MEDIA_EXPORT Status Transcode(const std::string& input_uri,
                                     const TranscodeParams& params,
                                     TranscodeProgressCB progress_cb = nullptr);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_FFMPEG_TRANSCODE_JOB_H_
