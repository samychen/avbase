// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The M8 pipeline wiring. What "ready" means here is deliberately minimal:
// the demuxer probed and the renderer's decoders initialized. Buffering
// high-water marks (M9), accurate-seek framing (M9) and track switching are
// still ahead; the corresponding interface methods fail loudly rather than
// pretending.

#include "media/filters/pipeline_impl.h"

#include "media/filters/live_edge_policy.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/default_tick_clock.h"
#include "media/base/native_display.h"

namespace avbase::media {

PipelineImpl::PipelineImpl() = default;

PipelineImpl::~PipelineImpl() {
  // The owner (Player::Impl) must have run Stop() and let the media sequence
  // tear the demuxer and renderer down first (docs/03 §10.1). Destroying them
  // here would race the demux thread and the sink callbacks.
  DCHECK(state_.load() == State::kNew || state_.load() == State::kStopped ||
         state_.load() == State::kError);
}

void PipelineImpl::SetSource(DataSourceDescriptor source,
                             DemuxerOptions options) {
  source_ = std::move(source);
  options_ = std::move(options);
}

void PipelineImpl::SetTickClock(const base::TickClock* clock) {
  tick_clock_ = clock ? clock : base::DefaultTickClock::GetInstance();
}

void PipelineImpl::SetClock(std::shared_ptr<AvSyncController> av_sync) {
  av_sync_ = std::move(av_sync);
}

void PipelineImpl::Start(
    std::unique_ptr<Demuxer> demuxer, RendererFactory* renderer_factory,
    RendererType renderer_type,
    base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
    Client* client) {
  DCHECK(demuxer);
  DCHECK(renderer_factory);
  DCHECK(client);
  demuxer_ = std::move(demuxer);
  renderer_factory_ = renderer_factory;
  client_ = client;
  media_runner_ = std::move(media_task_runner);
  // Posted, not run inline: the caller may be on any thread, and every piece
  // of media state below is media-sequence-exclusive.
  media_runner_->PostTask(FROM_HERE, base::BindOnce(&PipelineImpl::DoStart,

                                                    weak_factory_.GetWeakPtr(),
                                                    renderer_type));
}

void PipelineImpl::DoStart(RendererType renderer_type) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kNew) {
    return;
  }
  state_ = State::kStarting;

  // The demuxer doubles as the renderer's MediaResource (Demuxer derives
  // from it), and the renderer needs the probed stream configs, so the
  // demuxer initializes first and the renderer only starts on success --
  // firing both concurrently handed the renderer a demuxer with no streams.
  renderer_ = renderer_factory_->CreateRenderer(renderer_type, media_runner_);
  if (!renderer_) {
    // No factory could serve this type; report through the client rather
    // than leaving the pipeline half-started.
    state_ = State::kError;
    client_->OnError(
        PipelineStatusToMediaError(PipelineStatus::kFailedToCreatePipeline));
    return;
  }
  demuxer_->Initialize(source_, options_, this, media_runner_,
                       base::BindOnce(&PipelineImpl::OnDemuxerInitialized,

                                      weak_factory_.GetWeakPtr()));
}

void PipelineImpl::OnDemuxerInitialized(Status status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  demuxer_ready_ = status.has_value();
  if (!status) {
    state_ = State::kError;
    // The demuxer's Status carries the actionable MediaError already.
    client_->OnError(status.error());
    return;
  }
  // Demuxer ready: the renderer needs the probed stream configs, so only now
  // can it initialize against the demuxer's MediaResource view.
  //
  // Liveness is stated HERE, before Initialize(), because it is the first point
  // where it is known and the renderer cannot discover it: a MediaResource
  // hands out streams and nothing else. One call, one decision, rather than a
  // liveness flag threaded through the renderer's whole surface.
  renderer_->SetSourceLiveness(demuxer_->IsLive(), live_cue_max_age_);
  renderer_->Initialize(demuxer_.get(), this, media_runner_,
                        base::BindOnce(&PipelineImpl::OnRendererInitialized,

                                       weak_factory_.GetWeakPtr()));
  {
    base::AutoLock scoped(snapshot_lock_);
    media_info_ = demuxer_->media_info();
  }
  const MediaInfo info = demuxer_->media_info();
  if (info.duration != kNoTimestamp) {
    duration_micros_.store(info.duration.InMicroseconds());
    client_->OnDurationChange(info.duration);
  }
  seekable_.store(info.seekable && !info.is_live);
  MaybeReady();
}

void PipelineImpl::OnRendererInitialized(PipelineStatus status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  renderer_ready_ = status == PipelineStatus::kOk;
  if (!renderer_ready_) {
    state_ = State::kError;
    client_->OnError(PipelineStatusToMediaError(status));
    return;
  }
  MaybeReady();
}

void PipelineImpl::MaybeReady() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kStarting || !demuxer_ready_ || !renderer_ready_) {
    return;
  }
  state_ = State::kReady;
  // Pipeline::Start()'s contract reports readiness through the client's first
  // buffering/duration callbacks; kHaveMetadata is "probed, not yet playing".
  client_->OnBufferingStateChange(BufferingState::kHaveMetadata,
                                  base::TimeDelta());
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

void PipelineImpl::OnDemuxerStarted(Status status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!status) {
    LOG(WARNING) << "avbase.pipeline: demuxer start reported "
                 << status.error().summary();
  }
  if (state_ == State::kReady && renderer_) {
    renderer_->StartPlayingFrom(base::TimeDelta());
  }
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
        MediaError(ErrorCode::kInvalidState,
                   "the pipeline is not initialized",
                   "TakeSnapshot arrived before a successful prepare",
                   "wait for the kPrepared state"),
        nullptr);
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&PipelineImpl::DoTakeSnapshot,
                     weak_factory_.GetWeakPtr(), at, std::move(callback)));
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
    // Collapse: the in-flight seek keeps going and this one starts the moment
    // it finishes. The superseded completion still runs, so a caller matching
    // completions to requests is never left waiting (the facade owns the
    // request ids and its own supersede policy).
    pending_seek_superseded_ = true;
    std::move(seeked_cb).Run();
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
  demuxer_->StartPlayingFrom(time,
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
  }
  seek_demuxer_done_ = true;
  FinishSeekIfBothDone();
}

void PipelineImpl::FinishSeekIfBothDone() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!seek_in_flight_ || !seek_demuxer_done_ || !seek_renderer_flushed_) {
    return;
  }
  seek_in_flight_ = false;
  pending_seek_superseded_ = false;
  if (state_ == State::kReady && playing_ && renderer_) {
    renderer_->StartPlayingFrom(seek_time_);
  }
  if (seek_cb_) {
    std::move(seek_cb_).Run();
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
  if (demuxer_) {
    demuxer_->Stop();
    demuxer_.reset();
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
