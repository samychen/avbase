// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// PipelineImpl's STARTING half: SetSource, Start, DoStart and the two
// initialization callbacks that make the pipeline ready. pipeline_impl.cc
// keeps construction, the transport controls, seek and teardown.
//
// The split is a line-count one (C1), and the seam is chosen to match the ones
// this class already has (pipeline_impl_host.cc, _properties.cc,
// _track_select.cc): what moves is the whole "a pipeline comes into existence"
// sequence, which is contiguous -- DoStart hands the demuxer to the renderer,
// whose callbacks are what MaybeReady() joins. Nothing in it is called once
// the pipeline is running, which is what makes cutting here unable to split a
// lifetime.
//
// Everything here runs on the media sequence except SetSource/Start, which are
// pre-flight and run on the caller's thread -- and that is exactly the
// distinction Start()'s own comment spends twelve lines on.

#include "media/filters/pipeline_impl.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/time/default_tick_clock.h"

namespace avbase::media {

void PipelineImpl::SetSource(std::vector<DataSourceDescriptor> sources,
                             DemuxerOptions options) {
  descriptors_ = std::move(sources);
  options_ = std::move(options);
}

void PipelineImpl::SetTickClock(const base::TickClock* clock) {
  tick_clock_ = clock ? clock : base::DefaultTickClock::GetInstance();
}

void PipelineImpl::SetClock(std::shared_ptr<AvSyncController> av_sync) {
  av_sync_ = std::move(av_sync);
}

void PipelineImpl::Start(
    std::vector<std::unique_ptr<Demuxer>> sources,
    RendererFactory* renderer_factory, RendererType renderer_type,
    base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
    Client* client) {
  DCHECK(!sources.empty());
  DCHECK(renderer_factory);
  DCHECK(client);
  // The kNew -> kStarting transition happens HERE, on the calling thread,
  // not inside DoStart. IsRunning() is the "safe to destroy" predicate every
  // waiter polls -- Player::Impl::StopSync and the test teardowns -- and a
  // pipeline whose Start() has been posted but not yet run reads kNew, which
  // is NOT running. A host that prepares and immediately stops (open a URL,
  // change your mind) would then be told "safe" before the media sequence has
  // done anything: the teardown stops the media thread, DoStop is dropped
  // from its queue, and the pipeline is destroyed mid-kStarting --
  // ~PipelineImpl's DCHECK names exactly that.
  //
  // The exchange also keeps Start() start-once, the job DoStart's old kNew
  // guard did: a second Start() on a pipeline that already moved on is
  // refused before it can replace the members below.
  State expected = State::kNew;
  if (!state_.compare_exchange_strong(expected, State::kStarting)) {
    return;
  }
  sources_ = std::move(sources);
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
  // kStarting was set synchronously in Start(), so this is the "posted
  // exactly once" guard now. Anything else -- stopping, stopped, errored --
  // means starting would be wrong.
  if (state_ != State::kStarting) {
    return;
  }

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
  // Aggregate the N demuxers into a single MediaResource. Each child carries
  // its own descriptor -- descriptors_[i] opens sources_[i] -- and the single
  // |source|/|options| handed to Demuxer::Initialize below is ignored by the
  // CompositeDemuxer, which reads per-child bundles instead. For a
  // single-source Start descriptors_ is a one-element vector and this
  // reproduces the old direct path exactly.
  //
  // Index pairing is the contract, but Start() with no SetSource() at all is a
  // legal call today (the stage-0 multi-source tests exercise exactly that), so
  // an absent descriptor is PADDED rather than rejected: it means "this source
  // carries its own data", true of every synthetic demuxer. Anything that
  // really needs one -- FFmpegDemuxer opening a URI -- fails inside
  // Initialize with a real error, so padding cannot hide a bad open.
  if (descriptors_.size() < sources_.size()) {
    descriptors_.resize(sources_.size());
  }
  std::vector<CompositeDemuxer::Source> composite_sources;
  composite_sources.reserve(sources_.size());
  for (size_t i = 0; i < sources_.size(); ++i) {
    CompositeDemuxer::Source s;
    s.demuxer = std::move(sources_[i]);
    s.descriptor = descriptors_[i];
    s.options = options_;
    composite_sources.push_back(std::move(s));
  }
  sources_.clear();
  composite_ = std::make_unique<CompositeDemuxer>(std::move(composite_sources));

  composite_->Initialize(descriptors_.empty() ? DataSourceDescriptor{}
                                              : descriptors_[0],
                         options_, this, media_runner_,
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
  renderer_->SetSourceLiveness(composite_->IsLive(), live_cue_max_age_);
  renderer_->Initialize(composite_.get(), this, media_runner_,
                        base::BindOnce(&PipelineImpl::OnRendererInitialized,

                                       weak_factory_.GetWeakPtr()));
  // Composed ONCE and read twice: on a composite the answer is a full merge
  // (every StreamInfo copied, a metadata string per stream), so calling it
  // again for the snapshot below would pay for the same work twice.
  const MediaInfo info = composite_->media_info();
  {
    base::AutoLock scoped(snapshot_lock_);
    media_info_ = info;
  }
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
}  // namespace avbase::media
