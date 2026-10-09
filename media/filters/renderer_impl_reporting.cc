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
  if (video_ && snapshot.master_valid) {
    // Posted, not called: the compositor's write side must stay on S3 (see the
    // threading note in video_renderer_impl.h for why S3 and not S1).
    deps_.video_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(&VideoRendererImpl::SetMasterClock,
                       base::Unretained(video_.get()), snapshot.master,
                       av_sync_->master_serial(), true));
    deps_.video_task_runner->PostTask(
        FROM_HERE, base::BindOnce(&VideoRendererImpl::SetMasterIsVideo,
                                  base::Unretained(video_.get()),
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
  if (video_) {
    const VideoFrameCompositor::Stats c = video_->compositor_stats();
    stats.video_frames_presented = c.frames_presented;
    stats.video_frames_dropped = c.frames_dropped;
    stats.video_frames_repeated = c.frames_repeated;
    if (c.avg_av_diff_ms != 0.0) {
      stats.avg_av_diff_ms = c.avg_av_diff_ms;
    }
  }
  if (audio_) {
    stats.audio_underruns = audio_->underruns();
  }
  return stats;
}

void RendererImpl::TakeSnapshot(base::TimeDelta at,
                               Renderer::SnapshotFrameCallback callback) {
  if (!video_) {
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
  auto* video = video_.get();
  auto* media_runner = deps_.media_task_runner.get();
  deps_.video_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(
          [video, self, media_runner, at,
           callback = std::move(callback)]() mutable {
            if (!self) {
              return;
            }
            video->TakeSnapshot(
                at,
                base::BindOnce(
                    [self, media_runner,
                     callback = std::move(callback)](
                        MediaError error,
                        base::scoped_refptr<VideoFrame>
                            frame) mutable {
                      media_runner->PostTask(
                          FROM_HERE,
                          base::BindOnce(
                              [self, callback = std::move(callback)](
                                  MediaError error,
                        base::scoped_refptr<VideoFrame> frame) mutable {
                                if (self) {
                                  std::move(callback).Run(error,
                                                          std::move(frame));
                                }
                              },
                              error, std::move(frame)));
                    }));
          }));
}

}  // namespace avbase::media
