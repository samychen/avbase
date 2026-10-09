// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// PipelineImpl's public property surface: the four runtime setters (volume,
// rate, latency hint, preserves-pitch) and the read-back accessors (media
// time, buffered/duration, live-edge chase, statistics, seek capability).
// Split from pipeline_impl.cc as a line-count seam (C1): this is "the pipeline
// answering" -- every method here only reads or forwards to |renderer_| /
// |av_sync_| on the media sequence, none of it is the start/seek/stop
// lifecycle that the rest of the file carries.

#include "media/filters/pipeline_impl.h"

#include <utility>

#include "base/functional/bind.h"
#include "media/filters/live_edge_policy.h"

namespace avbase::media {

// The Set* family posts a task that reads |renderer_| on the media sequence
// rather than capturing renderer_.get() on the caller's thread: S1 may be
// tearing the renderer down concurrently with a call from any other thread.

// The four setters below bind `self` as a LAMBDA PARAMETER rather than as a
// member pointer, so they need the null check the member-function form gets
// from the binding layer for free: BindOnce unwraps a bound WeakPtr into a raw
// T* that is null once the factory has been destroyed. Same guard, different
// shape -- worth knowing before someone "simplifies" one into the other.
void PipelineImpl::SetVolume(float volume) {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(FROM_HERE, base::BindOnce(
                                         [](PipelineImpl* self, float v) {
                                           if (!self) {
                                             return;
                                           }
                                           if (self->renderer_) {
                                             self->renderer_->SetVolume(v);
                                           }
                                         },
                                         weak_factory_.GetWeakPtr(), volume));
}

void PipelineImpl::SetPlaybackRate(double rate) {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(FROM_HERE,
                          base::BindOnce(
                              [](PipelineImpl* self, double r) {
                                if (!self) {
                                  return;
                                }
                                if (self->renderer_) {
                                  self->renderer_->SetPlaybackRate(r);
                                }
                              },
                              weak_factory_.GetWeakPtr(), rate));
}

void PipelineImpl::SetLatencyHint(base::TimeDelta hint) {
  latency_hint_ = hint;
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(FROM_HERE,
                          base::BindOnce(
                              [](PipelineImpl* self, base::TimeDelta h) {
                                if (self && self->renderer_) {
                                  self->renderer_->SetLatencyHint(h);
                                }
                              },
                              weak_factory_.GetWeakPtr(), hint));
}

void PipelineImpl::SetPreservesPitch(bool preserves_pitch) {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(FROM_HERE,
                          base::BindOnce(
                              [](PipelineImpl* self, bool p) {
                                if (self && self->renderer_) {
                                  self->renderer_->SetPreservesPitch(p);
                                }
                              },
                              weak_factory_.GetWeakPtr(), preserves_pitch));
}

base::TimeDelta PipelineImpl::GetMediaTime() {
  if (!av_sync_) {
    return kNoTimestamp;
  }
  return av_sync_->GetMasterClock();
}

base::TimeDelta PipelineImpl::GetBufferedTime() const {
  return base::TimeDelta::FromMicroseconds(buffered_micros_.load());
}

base::TimeDelta PipelineImpl::GetDuration() const {
  return base::TimeDelta::FromMicroseconds(duration_micros_.load());
}

bool PipelineImpl::ShouldChaseToLiveEdge(base::TimeDelta behind) const {
  return LiveEdgePolicy::ShouldChase(true, latency_hint_, behind);
}

Pipeline::Statistics PipelineImpl::GetStatistics() const {
  Pipeline::Statistics out;
  base::AutoLock scoped(snapshot_lock_);
  out.buffered_time =
      base::TimeDelta::FromMicroseconds(buffered_micros_.load());
  out.duration = base::TimeDelta::FromMicroseconds(duration_micros_.load());
  out.total_bytes_read = last_stats_.total_bytes_read;
  out.video_frames_presented = last_stats_.video_frames_presented;
  out.video_frames_dropped = last_stats_.video_frames_dropped;
  out.audio_glitches = last_stats_.audio_glitches;
  out.avg_av_diff_ms = last_stats_.avg_av_diff_ms;
  out.seek_count = last_stats_.seek_count;
  return out;
}

bool PipelineImpl::CanSeekForward() const {
  return seekable_.load();
}

bool PipelineImpl::CanSeekBackward() const {
  return seekable_.load();
}

}  // namespace avbase::media
