// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_FFMPEG_CONCAT_JOB_H_
#define AVBASE_MEDIA_FILTERS_FFMPEG_CONCAT_JOB_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "media/base/media_error.h"
#include "media/media_export.h"

namespace avbase::media {

// Multi-input concatenation: the E4 slab. Mirrors combine.cc's CombineTask —
// each input segment is either fast-pathed (stream copy + bsf auto-insert)
// or slow-pathed (re-encode through Transcode), then stitched together with
// continuous timestamps.
//
// Design:
//   - Fast path (copy): when all inputs share the same codec parameters,
//     packets are copied directly. A bitstream filter (h264_mp4toannexb or
//     hevc_mp4toannexb) is auto-inserted when the codec requires annexb
//     format (e.g. concatenating MP4 H.264 into a matroska output).
//   - Slow path (re-encode): when parameters differ between inputs, the
//     segment is transcoded to match the first input's parameters before
//     concatenation.
//   - Timestamp rebase: each input's packets are rebased onto a continuous
//     output timeline starting at 0. The offset accumulates per-segment.
//
// Parameters:
//   |inputs|    — ordered list of input URIs to concatenate.
//   |output_path| — the output container (extension selects muxer).
//   |stream_select| — which streams to include (audio/video).
struct AVBASE_MEDIA_EXPORT ConcatParams {
  std::vector<std::string> inputs;
  std::string output_path;
  // Container options (same as TranscodeParams::muxer_options).
  std::vector<std::pair<std::string, std::string>> muxer_options;
  // If true, re-encode all segments (forces slow path). Default: false
  // (auto-select fast/slow per segment).
  bool force_reencode = false;
};

// Runs the concat synchronously. Returns OkStatus on success.
// The caller's thread blocks until the job completes or fails.
AVBASE_MEDIA_EXPORT Status Concat(const ConcatParams& params);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_FFMPEG_CONCAT_JOB_H_
