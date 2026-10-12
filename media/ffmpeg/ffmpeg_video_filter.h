// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_VIDEO_FILTER_H_
#define AVBASE_MEDIA_FFMPEG_VIDEO_FILTER_H_

#include <optional>
#include <string>

#include "media/base/render_perf_stats.h"
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
  const Size& CodedSize() const override;

  // Why Process() last returned false. Reset to nullopt on every call, so it
  // describes THAT call rather than any earlier one -- a refusal that is
  // still readable after a later success would be a lie.
  //
  // It exists because "Process returned false" alone cannot be tested
  // against: feeding null plane pointers to avfilter also fails, just later
  // and with the wrong story. Same vocabulary as VideoFilterState because a
  // refusal here and a refusal in the renderer are one fact seen from both
  // ends, and whoever has to explain "the filter is not running" should not
  // have to translate.
  std::optional<VideoFilterState> last_rejection() const {
    return last_rejection_;
  }

 private:
  struct Context;
  std::unique_ptr<Context> ctx_;
  std::optional<VideoFilterState> last_rejection_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FFMPEG_VIDEO_FILTER_H_
