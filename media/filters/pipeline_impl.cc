// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The M8 pipeline wiring: the transport controls (play/pause/snapshot), seek
// and teardown. The starting half -- SetSource/Start/DoStart and the two init
// callbacks -- lives in pipeline_impl_start.cc (C1 split).
//
// What "ready" means here is deliberately minimal: the demuxer probed and the
// renderer's decoders initialized. Buffering high-water marks (M9),
// accurate-seek framing (M9) and track switching are still ahead; the
// corresponding interface methods fail loudly rather than pretending.

#include "media/filters/pipeline_impl.h"

#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/base/native_display.h"
#include "media/filters/live_edge_policy.h"

namespace avbase::media {

PipelineImpl::PipelineImpl() = default;

PipelineImpl::~PipelineImpl() {
  // The owner (Player::Impl) must have run Stop() and let the media sequence
  // tear the demuxer and renderer down first (docs/03 §10.1). Destroying them
  // here would race the demux thread and the sink callbacks.
  DCHECK(state_.load() == State::kNew || state_.load() == State::kStopped ||
         state_.load() == State::kError);
}

void PipelineImpl::Play() {
  if (media_runner_) {
    media_runner_->PostTask(FROM_HERE,
                            base::BindOnce(&PipelineImpl::DoPlay,

                                           weak_factory_.GetWeakPtr()));
  }
}

void PipelineImpl::DoPlay() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kReady || !renderer_) {
    return;
  }
  if (!playing_) {
    playing_ = true;
    // No demuxer seek here: the demux loop has been reading from the start
    // position since Initialize() (backpressure-queued), so a seek to 0
    // would only flush and re-read what is already buffered. The renderer's
    // pumps start from the first buffered serial.
    renderer_->StartPlayingFrom(base::TimeDelta());
    return;
  }
  // Resuming a pause: the demuxer is already running; release the sinks.
  renderer_->SetPaused(false);
}

void PipelineImpl::Pause() {
  if (media_runner_) {
    media_runner_->PostTask(FROM_HERE,
                            base::BindOnce(&PipelineImpl::DoPause,

                                           weak_factory_.GetWeakPtr()));
  }
}

void PipelineImpl::DoPause() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::kReady && renderer_) {
    renderer_->SetPaused(true);
  }
}

void PipelineImpl::SetOutputTarget(base::scoped_refptr<NativeDisplay> display) {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(FROM_HERE,
                          base::BindOnce(&PipelineImpl::DoSetOutputTarget,

                                         weak_factory_.GetWeakPtr(),
                                         std::move(display)));
}

void PipelineImpl::TakeSnapshot(base::TimeDelta at,
                                Renderer::SnapshotFrameCallback callback) {
  if (!media_runner_) {
    std::move(callback).Run(
        MediaError(ErrorCode::kInvalidState, "the pipeline is not initialized",
                   "TakeSnapshot arrived before a successful prepare",
                   "wait for the kPrepared state"),
        nullptr);
    return;
  }
  media_runner_->PostTask(FROM_HERE,
                          base::BindOnce(&PipelineImpl::DoTakeSnapshot,
                                         weak_factory_.GetWeakPtr(), at,
                                         std::move(callback)));
}

void PipelineImpl::DoTakeSnapshot(base::TimeDelta at,
                                  Renderer::SnapshotFrameCallback callback) {
  if (renderer_) {
    renderer_->TakeSnapshot(at, std::move(callback));
  } else {
    std::move(callback).Run(
        MediaError(ErrorCode::kInvalidState, "the pipeline has no renderer",
                   "TakeSnapshot arrived before the renderer was created",
                   "wait for the kPrepared state"),
        nullptr);
  }
}
void PipelineImpl::DoSetOutputTarget(
    base::scoped_refptr<NativeDisplay> display) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (renderer_) {
    renderer_->SetOutputTarget(std::move(display));
  }
}

void PipelineImpl::BeginAccurateSeek(base::TimeDelta target,
                                     base::OnceClosure reached_cb) {
  if (!media_runner_) {
    std::move(reached_cb).Run();
    return;
  }
  media_runner_->PostTask(FROM_HERE,
                          base::BindOnce(&PipelineImpl::DoBeginAccurateSeek,

                                         weak_factory_.GetWeakPtr(), target,
                                         std::move(reached_cb)));
}

void PipelineImpl::DoBeginAccurateSeek(base::TimeDelta target,
                                       base::OnceClosure reached_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (renderer_) {
    renderer_->BeginAccurateSeek(target, std::move(reached_cb));
  }
}

void PipelineImpl::EndAccurateSeek() {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(FROM_HERE,
                          base::BindOnce(&PipelineImpl::DoEndAccurateSeek,
                                         weak_factory_.GetWeakPtr()));
}

void PipelineImpl::DoEndAccurateSeek() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (renderer_) {
    renderer_->EndAccurateSeek();
  }
}

void PipelineImpl::Seek(base::TimeDelta time, base::OnceClosure seeked_cb) {
  if (!media_runner_) {
    std::move(seeked_cb).Run();
    return;
  }
  media_runner_->PostTask(FROM_HERE,
                          base::BindOnce(&PipelineImpl::DoSeek,
                                         weak_factory_.GetWeakPtr(), time,
                                         std::move(seeked_cb)));
}

void PipelineImpl::DoSeek(base::TimeDelta time, base::OnceClosure seeked_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kReady || !renderer_) {
    std::move(seeked_cb).Run();
    return;
  }
  if (seek_in_flight_) {
    // Collapse: the in-flight seek keeps going, and THIS target starts the
    // moment it finishes. The newest user intent wins: a target queued by an
    // even earlier collapse is superseded here (its completion still runs, so
    // a caller matching completions to requests is never left waiting -- the
    // facade owns the request ids and its own supersede policy).
    if (pending_seek_cb_) {
      std::move(pending_seek_cb_).Run();
    }
    pending_seek_valid_ = true;
    pending_seek_time_ = time;
    pending_seek_cb_ = std::move(seeked_cb);
    return;
  }
  seek_in_flight_ = true;
  seek_time_ = time;
  seek_cb_ = std::move(seeked_cb);
  seek_demuxer_done_ = false;
  seek_renderer_flushed_ = false;
  // Flush the renderer and seek the demuxer in parallel (docs/04 §4.1); only
  // when both have finished is it safe to restart rendering, or a post-seek
  // frame could be scheduled against a pre-seek clock anchor.
  renderer_->Flush(base::BindOnce(&PipelineImpl::OnRendererFlushed,
                                  weak_factory_.GetWeakPtr()));
  composite_->StartPlayingFrom(time,
                               base::BindOnce(&PipelineImpl::OnSeekDemuxerDone,
                                              weak_factory_.GetWeakPtr()));
}

void PipelineImpl::OnRendererFlushed() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  seek_renderer_flushed_ = true;
  FinishSeekIfBothDone();
}

void PipelineImpl::OnSeekDemuxerDone(Status status, base::TimeDelta actual) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!status) {
    LOG(WARNING) << "avbase.pipeline: seek reported "
                 << status.error().summary() << " at " << actual.InSecondsF()
                 << "s";
  } else {
    // Before FinishSeekIfBothDone, which restarts rendering: the first
    // post-seek frames must already pace against the per-leg clocks, or
    // they would be scheduled against clocks the offsets have not reached.
    ApplySourceOffsets();
  }
  seek_demuxer_done_ = true;
  FinishSeekIfBothDone();
}

void PipelineImpl::ApplySourceOffsets() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!renderer_ || !composite_) {
    return;
  }
  // Stage 2 (docs/14 §7.2): every source's seek lands where ITS keyframe
  // geometry says, and those landings differ -- a secondary source's
  // interval is its own. The landing DIFFERENCE is the playback offset: the
  // moment source i's own timeline calls actual_i must composite as the same
  // master moment as source 0's actual_0, and SourceTime(i, pts) = pts +
  // offset_i, so offset_i = actual_0 - actual_i makes the two agree.
  //
  // Source 0 is never stated: it IS the master timeline, and shifting it
  // would correct the same skew twice (the controller refuses it anyway).
  const std::vector<base::TimeDelta>& actuals = composite_->seek_actuals();
  // The loop below starts at i=1, so a single source is skipped WITHOUT this
  // guard; its real job is to keep the actuals[0] read (the primary landing,
  // just below) off the end of an empty vector. Source 0's offset is zero by
  // construction in both cases, so returning early here changes nothing for it.
  if (actuals.size() < 2) {
    return;
  }
  const base::TimeDelta primary = actuals[0];
  for (size_t i = 1; i < actuals.size(); ++i) {
    renderer_->SetSourceOffset(static_cast<int>(i), primary - actuals[i]);
  }
}

void PipelineImpl::FinishSeekIfBothDone() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!seek_in_flight_ || !seek_demuxer_done_ || !seek_renderer_flushed_) {
    return;
  }
  seek_in_flight_ = false;
  if (state_ == State::kReady && playing_ && renderer_ &&
      !pending_seek_valid_) {
    // Skipped when a queued seek will restart rendering in a moment anyway:
    // starting here too would schedule one doomed post-seek frame batch.
    renderer_->StartPlayingFrom(seek_time_);
  }
  if (seek_cb_) {
    std::move(seek_cb_).Run();
  }
  // H3: the seek that arrived mid-flight used to be dropped on the floor (a
  // flag was set and never read). Start it now; DoSeek re-enters this whole
  // state machine with the queued target.
  if (pending_seek_valid_) {
    pending_seek_valid_ = false;
    DoSeek(pending_seek_time_, std::move(pending_seek_cb_));
  }
}

void PipelineImpl::Stop() {
  if (!media_runner_) {
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&PipelineImpl::DoStop, weak_factory_.GetWeakPtr()));
}

void PipelineImpl::DoStop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::kStopped || state_ == State::kStopping) {
    return;
  }
  state_ = State::kStopping;
  playing_ = false;
  // A seek completion can never arrive past this point: the flush below
  // closes the demuxer->S1 reply chain, and FinishSeekIfBothDone runs on that
  // chain. Drop the bookkeeping -- including the queued completion closures,
  // which name internals that are about to be torn down -- instead of
  // stranding it. The facade (PlayerImpl) owns the user-facing semantics and
  // answers its parked SeekCBs with kAborted (AbortPendingSeeks).
  seek_in_flight_ = false;
  pending_seek_valid_ = false;
  seek_cb_.Reset();
  pending_seek_cb_.Reset();
  // Order (docs/03 §10.1): renderer first -- destroying it stops both sinks,
  // which guarantees no further Render() calls -- then the demuxer, whose
  // Stop() interrupts the demux thread's blocking av_read_frame and joins it.
  //
  // Flushed, not just dropped: the Flush completes every in-flight decoder
  // read inline and closes the demuxer->S1->S4 reply chain, so no task
  // naming renderer internals can be queued after destruction. A bare reset
  // let a read reply land on a freed AudioRendererImpl (text-leg tests).
  if (renderer_) {
    renderer_->Flush(
        base::BindOnce(&PipelineImpl::FinishStop, weak_factory_.GetWeakPtr()));
    return;
  }
  FinishStop();
}

void PipelineImpl::FinishStop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  renderer_.reset();
  if (composite_) {
    composite_->Stop();
    composite_.reset();
  }
  state_ = State::kStopped;
}

bool PipelineImpl::IsRunning() const {
  const State s = state_.load();
  // kStopping counts: Stop() completes asynchronously (the renderer flush
  // hops back before the teardown), and every waiter on "!IsRunning()" --
  // the test teardowns, Player::StopSync's polling -- means "safe to
  // destroy", which is only true at kStopped.
  return s == State::kStarting || s == State::kReady || s == State::kStopping;
}

}  // namespace avbase::media
