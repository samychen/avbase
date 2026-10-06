// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_VIDEO_FILTER_STAGE_H_
#define AVBASE_MEDIA_FILTERS_VIDEO_FILTER_STAGE_H_

#include <memory>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "media/base/video_frame.h"
#include "media/media_export.h"

namespace avbase::media {

// One video filter-graph stage. Pure interface (G2): the FFmpeg
// implementation is injected by the host, same pattern as
// AudioFilterStage. Frames in/out share the format the stage was built
// for; graphs that change geometry must append their own scale/crop tail
// and are rejected unless the sink is constrained accordingly.
class AVBASE_MEDIA_EXPORT VideoFilterStage {
 public:
  virtual ~VideoFilterStage() = default;

  virtual bool Initialize(const std::string& graph, VideoFormat format,
                          const Size& coded_size) = 0;

  // One frame in, zero or one frame out (avfilter may buffer). Null in →
  // null out. False on internal failure.
  virtual bool Process(base::scoped_refptr<VideoFrame> in,
                       base::scoped_refptr<VideoFrame>* out) = 0;
};

using VideoFilterStageFactory =
    std::function<std::unique_ptr<VideoFilterStage>()>;

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_VIDEO_FILTER_STAGE_H_
