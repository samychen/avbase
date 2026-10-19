// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// RendererImpl's outward-reporting surface: the master-clock push loop
// (PushMasterClock), the periodic statistics sampler (PushStatistics /
// GetStatistics), and the S3->S1 snapshot grab (TakeSnapshot). Split from
// renderer_impl.cc as a line-count seam (C1): these read av_sync_ and the
// sub-renderers and report outward -- they are not part of the
// initialize/flush/ended lifecycle that the rest of the file carries. Pushing
// them out keeps renderer_impl.cc below its 600-line ceiling without
// duplicating the video/audio state machine the file's allowlist note warns
// against splitting.

#include "media/filters/renderer_impl.h"

#include "base/functional/bind.h"
#include "base/logging.h"

namespace avbase::media {

void RendererImpl::PushMasterClock() {
  if (!initialized_ || ended_) {
    // A starvation left open by the end of playback must not outlive it.
    if (starved_reported_) {
      starved_reported_ = false;
      if (client_) {
        client_->OnBufferingStateChange(BufferingState::kHaveEnough,
                                        base::TimeDelta());
      }
    }
    clock_push_scheduled_ = false;
    return;
  }
  // Re-check completion while the clock loop runs: the ended hops fire at
  // decoder EOS, but "ended" also needs the buffers to drain, and nobody else
  // re-evaluates that.
  CheckForEnded();
  CheckBufferingTransitions();
  const AvSyncController::Snapshot snapshot = av_sync_->GetSnapshot();
  // Pushed to EVERY leg (docs/14 stage 1): a compositor with no clock
  // presents frames the instant they decode, which burns the decode chain
  // and grows the pending queue without bound. Source 0's leg remains the
  // only one whose presented frames feed the clock (WireVideoCallbacks).
  //
  // Each leg gets the master clock expressed in ITS OWN timeline (stage 2):
  // the compositor compares a frame's pts against the clock it is given, so
  // pacing leg i against (master - offset_i) presents a frame when the
  // master reaches SourceTime(i, pts) -- the frame's mapped position. That
  // is the inverse direction of SourceTime(), and the design's own formula
  // (§5.1: "present_time = master_now - source_offset"). Source 0's offset
  // is structurally zero, so the single-source push is unchanged to the
  // microsecond.
  for (size_t i = 0; i < video_renderers_.size(); ++i) {
    VideoRendererImpl* v = video_renderers_[i].get();
    if (!v || !snapshot.master_valid) {
      continue;
    }
    const base::TimeDelta leg_clock =
        snapshot.master - av_sync_->source_offset(static_cast<int>(i));
    // Posted, not called: the compositor's write side must stay on S3 (see the
    // threading note in video_renderer_impl.h for why S3 and not S1).
    deps_.video_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&VideoRendererImpl::SetMasterClock, base::Unretained(v),
                       leg_clock, av_sync_->master_serial(), true));
    deps_.video_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&VideoRendererImpl::SetMasterIsVideo,
                                  base::Unretained(v),
                                  snapshot.resolved ==
                                      AvSyncController::MasterType::kVideo));
  }
  deps_.media_task_runner->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&RendererImpl::PushMasterClock,
                     weak_factory_.GetWeakPtr()),
      kClockPushInterval);
}

void RendererImpl::PushStatistics() {
  if (!initialized_ || ended_) {
    stats_scheduled_ = false;
    return;
  }
  if (client_) {
    client_->OnStatisticsUpdate(GetStatistics());
  }
  deps_.media_task_runner->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&RendererImpl::PushStatistics, weak_factory_.GetWeakPtr()),
      kStatsInterval);
}

PipelineStatistics RendererImpl::GetStatistics() const {
  PipelineStatistics stats;
  if (av_sync_) {
    const AvSyncController::Snapshot s = av_sync_->GetSnapshot();
    stats.avg_av_diff_ms = s.av_diff.InMillisecondsF();
    if (audio_ && audio_params_.sample_rate() > 0) {
      stats.buffered_time =
          base::SecondsD(static_cast<double>(audio_->buffered_frames()) /
                         audio_params_.sample_rate());
    }
  }
  // Frame counts aggregate across every leg (docs/14 stage 1): the pipeline
  // presented N frames in total, and a second chain's frames are as real as
  // the first's. The per-frame COST and the A/V diff come from the FIRST leg
  // that has one -- the composited layer, the one CreateSubRenderers handed
  // the host sink, and the one whose lateness the viewer actually sees.
  bool primary_sampled = false;
  for (const auto& v : video_renderers_) {
    if (!v) {
      continue;
    }
    const VideoFrameCompositor::Stats c = v->compositor_stats();
    stats.video_frames_presented += c.frames_presented;
    stats.video_frames_dropped += c.frames_dropped;
    stats.video_frames_dropped_fps += c.frames_dropped_fps;
    stats.video_frames_dropped_late += c.frames_dropped_late;
    stats.video_frames_dropped_stale_serial += c.frames_dropped_stale_serial;
    stats.video_frames_dropped_accurate_seek += c.frames_dropped_accurate_seek;
    stats.video_frames_repeated += c.frames_repeated;
    // The display layer's half, summed for the same reason as the compositor's
    // counts above: a second leg's refused frames are as real as the first's.
    // Read here rather than inside the primary-only block below, because the
    // counters are per-leg while only the COST sample is primary-only.
    const VideoSinkStats sink = v->sink_stats();
    stats.video_frames_dropped_by_sink += sink.frames_dropped;
    stats.video_frames_submit_failures += sink.submit_failures;
    if (primary_sampled) {
      continue;
    }
    primary_sampled = true;
    if (c.avg_av_diff_ms != 0.0) {
      stats.avg_av_diff_ms = c.avg_av_diff_ms;
    }
    stats.measured_refresh_hz = sink.measured_refresh_hz;
    // Frame counts come from the compositor, which counts them; the
    // per-frame COST comes from the sink, which is the only object that
    // runs the GL calls. Sampling it here is what turns sink telemetry into
    // something a PlaybackStats reader can see, instead of a struct nobody
    // calls.
    //
    // filter_state is the one field the sink cannot supply. It sees only
    // the frames that came out the far end of the graph, so an unfiltered
    // frame and a frame the graph refused are the same thing to it; only
    // the renderer, which decided, can tell them apart.
    RenderPerfStats render = sink.render;
    render.filter_state = v->filter_state();
    stats.render = render;
  }
  if (audio_) {
    stats.audio_underruns = audio_->underruns();
  }
  return stats;
}

void RendererImpl::TakeSnapshot(base::TimeDelta at,
                                Renderer::SnapshotFrameCallback callback) {
  // Source 0's leg: the composited layer is what a snapshot of "the video"
  // means (docs/14 stage 1 -- a second picture is not yet composited in).
  if (!video_renderer()) {
    std::move(callback).Run(
        MediaError(ErrorCode::kNotImplemented,
                   "this pipeline has no video renderer",
                   "TakeSnapshot on an audio-only source",
                   "only request snapshots while a video track is selected"),
        nullptr);
    return;
  }
  // S3 grab, S1 deliver. The lambdas carry the WeakPtr as CAPTURED state --
  // binding a WeakPtr as an argument would unwrap it to a raw pointer at the
  // dispatch layer, losing the liveness check (see base/functional/bind.h).
  base::WeakPtr<RendererImpl> self = weak_factory_.GetWeakPtr();
  auto* video = video_renderer();
  auto* media_runner = deps_.media_task_runner.get();
  deps_.video_task_runner->PostTask(
      FROM_HERE, base::BindOnce([video, self, media_runner, at,
                                 callback = std::move(callback)]() mutable {
        if (!self) {
          return;
        }
        video->TakeSnapshot(
            at,
            base::BindOnce([self, media_runner, callback = std::move(callback)](
                               MediaError error,
                               base::scoped_refptr<VideoFrame> frame) mutable {
              media_runner->PostTask(
                  FROM_HERE,
                  base::BindOnce(
                      [self, callback = std::move(callback)](
                          MediaError error,
                          base::scoped_refptr<VideoFrame> frame) mutable {
                        if (self) {
                          std::move(callback).Run(error, std::move(frame));
                        }
                      },
                      error, std::move(frame)));
            }));
      }));
}

}  // namespace avbase::media
