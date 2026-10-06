// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_AUDIO_FILTER_STAGE_H_
#define AVBASE_MEDIA_FILTERS_AUDIO_FILTER_STAGE_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "media/base/audio_buffer.h"
#include "media/media_export.h"

namespace avbase::media {

// One audio filter-graph stage between the decoder and the render algorithm.
// Pure interface: the FFmpeg implementation (FFmpegAudioFilter) lives behind
// the platform layer and is injected by the host, so avbase_media stays
// buildable without FFmpeg (G2). Formats in/out are the decoded stream's
// planar float; a graph that changes them must append its own
// aresample+aformat tail.
class AVBASE_MEDIA_EXPORT AudioFilterStage {
 public:
  virtual ~AudioFilterStage() = default;

  // Builds the graph (an avfilter chain without buffer endpoints). False on
  // parse/config failure, with the reason in the log.
  virtual bool Initialize(const std::string& graph, int sample_rate,
                          int channels, int frames_per_hint) = 0;

  // Processes one buffer; EOS in flushes the graph and comes back as EOS in
  // |out|. False on internal failure only.
  virtual bool Process(base::scoped_refptr<AudioBuffer> in,
                       std::vector<base::scoped_refptr<AudioBuffer>>* out) = 0;
};

// Injected by the FFmpeg-enabled host; empty (null return) means the "af"
// config is ignored with a warning.
using AudioFilterStageFactory =
    std::function<std::unique_ptr<AudioFilterStage>()>;

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_AUDIO_FILTER_STAGE_H_
