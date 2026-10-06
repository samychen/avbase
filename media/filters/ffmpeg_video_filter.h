// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_FFMPEG_VIDEO_FILTER_H_
#define AVBASE_MEDIA_FILTERS_FFMPEG_VIDEO_FILTER_H_

#include <string>

#include "media/base/video_frame.h"
#include "media/filters/video_filter_stage.h"

namespace avbase::media {

// avfilter-backed VideoFilterStage: the "vf" half of the filter-graph
// capability. Constrains the sink to the input format and geometry, so a
// graph that would change them fails at Initialize instead of drifting.
class FFmpegVideoFilter final : public VideoFilterStage {
 public:
  FFmpegVideoFilter();
  FFmpegVideoFilter(const FFmpegVideoFilter&) = delete;
  FFmpegVideoFilter& operator=(const FFmpegVideoFilter&) = delete;
  ~FFmpegVideoFilter() override;

  bool Initialize(const std::string& graph, VideoFormat format,
                  const Size& coded_size) override;
  bool Process(base::scoped_refptr<VideoFrame> in,
               base::scoped_refptr<VideoFrame>* out) override;

 private:
  struct Context;
  std::unique_ptr<Context> ctx_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_FFMPEG_VIDEO_FILTER_H_
