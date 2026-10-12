// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_VIDEO_FILTER_STAGE_H_
#define AVBASE_MEDIA_FILTERS_VIDEO_FILTER_STAGE_H_

#include <memory>
#include <optional>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "media/base/render_perf_stats.h"
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

  // The geometry the stage was built for; the renderer rebuilds the stage
  // when a frame arrives with a different one (resolution change, Δ3).
  virtual const Size& CodedSize() const = 0;
};

using VideoFilterStageFactory =
    std::function<std::unique_ptr<VideoFilterStage>()>;

// Owns one stage's lifetime: when it is built, when it is torn down, and --
// the part that had no answer before -- what happens while the frames being
// decoded carry no CPU planes to filter.
//
// A CPU filter graph and a hardware-decoded frame are mutually exclusive:
// VideoFrame::visible_data() is empty for kStorageOpaque, so the graph is
// handed null plane pointers and rejects every frame. That used to surface
// as "buffersrc rejected a frame", which blames FFmpeg for what is really a
// configuration conflict -- and the failure return dropped the pump,
// freezing the picture with nothing reported.
//
// The refusal is therefore made BEFORE the graph exists, and named. It is
// deliberately not bridged by calling ToI420() here: that would drag every
// frame back to system memory and quietly undo the zero-copy path, which is
// the most expensive way to produce a picture that never needed to move.
class AVBASE_MEDIA_EXPORT VideoFilterStageRunner {
 public:
  // An empty |graph| means no filtering was requested. A graph with no
  // |factory| (a no-ffmpeg build) is refused HERE rather than at the first
  // frame, because with no factory there is no stage to build, now or ever.
  VideoFilterStageRunner(std::string graph, VideoFilterStageFactory factory);

  // Hands |frame| to the stage, or leaves it alone when no stage is running.
  // Returns false only when the stage swallowed the frame to buffer it
  // (EAGAIN) and there is genuinely nothing to show yet; |*frame| is then
  // empty. A refusal costs the graph, never the picture.
  bool Filter(base::scoped_refptr<VideoFrame>* frame);

  // Drops the stage but keeps the request: a flush discards the frames the
  // graph has buffered -- every one belongs to the generation being thrown
  // away -- not the configuration. The next mappable frame rebuilds.
  //
  // The hardware refusal is re-armed here too, so the notice is once per
  // stream generation and not once per renderer: a seek is a deliberate act
  // and may land on different material.
  void Reset() {
    stage_.reset();
    hardware_notice_logged_ = false;
  }

  // Whether the graph is being applied, and if not, why; see the enum.
  VideoFilterState state() const { return state_; }

 private:
  std::string graph_;
  VideoFilterStageFactory factory_;
  std::unique_ptr<VideoFilterStage> stage_;
  VideoFilterState state_{VideoFilterState::kOff};
  bool hardware_notice_logged_{false};
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_VIDEO_FILTER_STAGE_H_
