// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.

#include "media/filters/video_filter_stage.h"

#include <utility>

#include "base/logging.h"

namespace avbase::media {

VideoFilterStageRunner::VideoFilterStageRunner(std::string graph,
                                               VideoFilterStageFactory factory)
    : graph_(std::move(graph)), factory_(std::move(factory)) {
  if (graph_.empty()) {
    return;  // Nothing was requested; state_ stays kOff.
  }
  if (!factory_) {
    LOG(ERROR) << "avbase.vfilter: filter_graph set but no stage factory was "
                  "injected (no-ffmpeg build?); playing unfiltered";
    graph_.clear();
    state_ = VideoFilterState::kRefusedNoFactory;
    return;
  }
  // Optimistic until the first frame: a graph is only known to be running
  // once a frame has been through it, and it cannot be built before one
  // arrives -- it needs the real coded geometry, and planes it can read. The
  // first frame corrects this either way, so nothing can sit in the wrong
  // state for longer than one frame.
  state_ = VideoFilterState::kRunning;
}

bool VideoFilterStageRunner::Filter(base::scoped_refptr<VideoFrame>* frame) {
  if (graph_.empty() || !frame || !*frame) {
    return true;  // no filtering asked for: whatever arrived is shown.
  }
  // Drop the stage if the geometry moved (Δ3) or the storage stopped being
  // mappable. Dropped rather than fed, so a decoder that later falls back to
  // software picks the graph up again on its own.
  if (stage_ && (stage_->CodedSize() != (*frame)->coded_size() ||
                 !(*frame)->IsMappable())) {
    stage_.reset();
  }

  if (!stage_) {
    if (!(*frame)->IsMappable()) {
      // Refused, and said once per stream rather than once per frame: a log
      // that repeats at frame rate is a log nobody reads.
      state_ = VideoFilterState::kRefusedHardwareFrame;
      if (!hardware_notice_logged_) {
        LOG(ERROR) << "avbase.vfilter: graph \"" << graph_
                   << "\" filters CPU planes but the decoder is producing "
                      "hardware frames (zero-copy); showing the picture "
                      "unfiltered. Disable hwaccel to apply the graph.";
        hardware_notice_logged_ = true;
      }
      return true;
    }
    stage_ = factory_();
    if (!stage_->Initialize(graph_, (*frame)->format(),
                            (*frame)->coded_size())) {
      LOG(ERROR) << "avbase.vfilter: graph \"" << graph_
                 << "\" failed to start; playing unfiltered";
      stage_.reset();
      graph_.clear();
      state_ = VideoFilterState::kRefusedGraphFailed;
      return true;
    }
    state_ = VideoFilterState::kRunning;
  }

  base::scoped_refptr<VideoFrame> filtered;
  // Process() takes the frame by value, so hold our own reference across the
  // call: a stage that fails would otherwise take the picture with it, and
  // what is left to show is the frame that went in.
  base::scoped_refptr<VideoFrame> input = *frame;
  if (!stage_->Process(std::move(input), &filtered)) {
    LOG(ERROR) << "avbase.vfilter: dropping the filter stage after a "
                  "processing failure; playing unfiltered";
    stage_.reset();
    graph_.clear();
    state_ = VideoFilterState::kRefusedGraphFailed;
    return true;
  }
  if (!filtered) {
    // EAGAIN: the graph is still holding this frame, so there is nothing to
    // show right now. Emptied rather than left alone because "keep showing
    // the previous frame" is the caller's decision to make, not ours.
    frame->reset();
    return false;
  }
  *frame = std::move(filtered);
  return true;
}

}  // namespace avbase::media
