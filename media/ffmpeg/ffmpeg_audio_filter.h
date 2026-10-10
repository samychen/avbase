// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_AUDIO_FILTER_H_
#define AVBASE_MEDIA_FFMPEG_AUDIO_FILTER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "media/base/audio_buffer.h"
#include "media/filters/audio_filter_stage.h"

namespace avbase::media {

// One audio filter-graph stage, driven by the "af" filter_graph config.
//
// Ported role from MediaComponent's framework/ffmpeg/audio_filter.h, rewritten
// onto avbase's AudioBuffer contract: buffers in (matching the stream config),
// buffers out (the graph's negotiated output). Requires avfilter in the
// FFmpeg build (tools/setup_ffmpeg.sh enables it and the common audio
// filters).
class FFmpegAudioFilter final : public AudioFilterStage {
 public:
  FFmpegAudioFilter();
  FFmpegAudioFilter(const FFmpegAudioFilter&) = delete;
  FFmpegAudioFilter& operator=(const FFmpegAudioFilter&) = delete;
  ~FFmpegAudioFilter();

  // Builds the graph. |graph| is an avfilter chain without the buffer/sink
  // endpoints ("volume=0.5,aecho=0.8:0.8:60:0.4"). The input format is the
  // decoded stream's; the output format is whatever the graph negotiates
  // (aformat/anull pass it through; aresample would change it). Returns
  // false with the reason in the log when the graph fails to parse.
  bool Initialize(const std::string& graph, int sample_rate, int channels,
                  int frames_per_hint) override;

  bool Process(base::scoped_refptr<AudioBuffer> in,
               std::vector<base::scoped_refptr<AudioBuffer>>* out) override;

 private:
  struct Context;
  std::unique_ptr<Context> ctx_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FFMPEG_AUDIO_FILTER_H_
