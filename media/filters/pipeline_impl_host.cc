// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// PipelineImpl's receiving half: the stream-id queries, the track-switch stubs
// and the Demuxer::Host / RendererClient callbacks that the demuxer and the
// renderer push back on the media sequence. pipeline_impl.cc keeps the
// lifecycle (Start/Play/Pause/Seek/Stop) and the Set* family; the split is a
// line-count one (C1), and the seam is "who calls this" -- nothing here is
// driven by the facade's own state machine.

#include "media/filters/pipeline_impl.h"

#include <memory>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/filters/live_edge_policy.h"

namespace avbase::media {

namespace {

int StreamIdFor(Demuxer* demuxer, DemuxerStreamType type) {
  const DemuxerStream* stream = demuxer ? demuxer->GetStream(type) : nullptr;
  return stream ? stream->stream_index() : -1;
}

}  // namespace

int PipelineImpl::GetAudioStreamId() const {
  return StreamIdFor(composite_.get(), DemuxerStreamType::kAudio);
}

int PipelineImpl::GetVideoStreamId() const {
  return StreamIdFor(composite_.get(), DemuxerStreamType::kVideo);
}

int PipelineImpl::GetTextStreamId() const {
  // TextRenderer is deliberately absent (M7 decision, gap 3 in
  // renderer_impl.h): an honest -1 instead of a track that does nothing.
  return -1;
}

// These six reject, and the reason is NOT that they are unimplemented -- see
// the block above AddVideoStream in media/base/pipeline.h. They name a shape
// avbase does not have: `id` would select among several streams of one type,
// and Demuxer::GetStream(type) returns ONE per type. A second picture is a
// second URL (a second Demuxer, or a second Player feeding the scene's
// mailbox), not an id.
//
// Two things the previous comment asserted that are not true, and which
// mattered because they made this look like a queue:
//   * "M9 work" -- M9 is long past; nothing was ever wired.
//   * "the facade rejects them with kNotImplemented before reaching here" --
//     grepping the player/ tree for any of these names finds nothing, so the
//     facade neither rejects them nor calls them. They are simply unreachable.
//
// WARNING rather than DCHECK: the return type is void and the pipeline may be
// called before it is ready, so a caller that does reach here gets a visible
// diagnostic and no effect -- which is exactly what the interface promises.
void PipelineImpl::AddVideoStream(int id) {
  LOG(WARNING) << "avbase.pipeline: AddVideoStream(" << id
               << ") rejected: avbase has one stream per type, not an id axis";
}
void PipelineImpl::RemoveVideoStream(int id) {
  LOG(WARNING) << "avbase.pipeline: RemoveVideoStream(" << id
               << ") rejected: avbase has one stream per type, not an id axis";
}
void PipelineImpl::AddAudioStream(int id) {
  LOG(WARNING) << "avbase.pipeline: AddAudioStream(" << id
               << ") rejected: avbase has one stream per type, not an id axis";
}
void PipelineImpl::RemoveAudioStream(int id) {
  LOG(WARNING) << "avbase.pipeline: RemoveAudioStream(" << id
               << ") rejected: avbase has one stream per type, not an id axis";
}
void PipelineImpl::AddTextStream(int id) {
  LOG(WARNING) << "avbase.pipeline: AddTextStream(" << id
               << ") rejected: avbase has one stream per type, not an id axis";
}
void PipelineImpl::RemoveTextStream(int id) {
  LOG(WARNING) << "avbase.pipeline: RemoveTextStream(" << id
               << ") rejected: avbase has one stream per type, not an id axis";
}

// ---- Demuxer::Host ----------------------------------------------------------

void PipelineImpl::SetDuration(base::TimeDelta duration) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  duration_micros_.store(duration.InMicroseconds());
  client_->OnDurationChange(duration);
}

void PipelineImpl::OnBufferedTimeUpdate(base::TimeDelta buffered,
                                        base::TimeDelta playback_time) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  buffered_micros_.store(buffered.InMicroseconds());
  (void)playback_time;
  // Deliberately not forwarded as an event: Pipeline::Client carries buffered
  // time through OnStatisticsUpdate, and the facade polls GetBufferedTime()
  // on its stats tick. An event per demux progress callback would flood the
  // event queue on network sources.
}

void PipelineImpl::OnDemuxerError(MediaError error) {
  state_ = State::kError;
  client_->OnError(std::move(error));
}

// ---- RendererClient ---------------------------------------------------------

void PipelineImpl::OnError(MediaError error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  state_ = State::kError;
  client_->OnError(std::move(error));
}

void PipelineImpl::OnEnded() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  playing_ = false;
  client_->OnEnded();
}

void PipelineImpl::OnBufferingStateChange(BufferingState state,
                                          base::TimeDelta memory_usage) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  client_->OnBufferingStateChange(state, memory_usage);
}

void PipelineImpl::OnWaiting(WaitingReason reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  client_->OnWaiting(reason);
}

void PipelineImpl::OnDurationChange(base::TimeDelta duration) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  duration_micros_.store(duration.InMicroseconds());
  client_->OnDurationChange(duration);
}

void PipelineImpl::OnStatisticsUpdate(const PipelineStatistics& stats) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  PipelineStatistics merged = stats;
  if (composite_) {
    const DemuxerStats d = composite_->GetStats();
    merged.total_bytes_read = d.bytes_read;
    merged.seek_count = d.seek_count;
  }
  {
    base::AutoLock scoped(snapshot_lock_);
    last_stats_ = merged;
  }
  // Live-edge chase (docs/12 section 2.1), evaluated on the 1 s statistics
  // tick: behind = live edge (a live stream's growing duration) minus
  // playhead. The action is the pipeline's own two-phase skip (renderer
  // flush + demuxer resume-from-newest), NOT a physical seek -- live
  // containers cannot seek backward.
  if (composite_ && composite_->IsLive() && playing_ && !seek_in_flight_) {
    const base::TimeDelta edge =
        base::TimeDelta::FromMicroseconds(duration_micros_.load());
    const base::TimeDelta behind = edge - GetMediaTime();
    if (ShouldChaseToLiveEdge(behind)) {
      const base::TimeDelta target =
          LiveEdgePolicy::SuggestedTarget(edge, latency_hint_);
      LOG(WARNING) << "ijkpp.pipeline: live chase, behind="
                   << behind.InMillisecondsF() << "ms, skipping to "
                   << target.InSecondsF() << "s";
      seek_in_flight_ = true;
      seek_time_ = target;
      seek_demuxer_done_ = false;
      seek_renderer_flushed_ = false;
      // M12: weak handles, same as every other cross-hop in this file -- the
      // callbacks can outlive the pipeline if it is Reset() while the live
      // chase is launching (pipeline_impl.cc's Seek path already does this).
      renderer_->Flush(base::BindOnce(&PipelineImpl::OnRendererFlushed,
                                      weak_factory_.GetWeakPtr()));
      composite_->StartPlayingFrom(
          target, base::BindOnce(&PipelineImpl::OnSeekDemuxerDone,
                                 weak_factory_.GetWeakPtr()));
    }
  }
  client_->OnStatisticsUpdate(merged);
}

void PipelineImpl::OnTimedText(const TimedTextCue& cue) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  client_->OnTimedText(cue);
}

void PipelineImpl::OnVideoConfigChange(const VideoDecoderConfig& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  client_->OnVideoConfigChange(config);
}

void PipelineImpl::OnAudioOutputDeviceChanged(const std::string& device_id,
                                              bool is_default,
                                              OutputDeviceStatus status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Pipeline::Client has no device-change hook (it is a RendererClient
  // member); diagnostics get it via the log until M9's device switching
  // defines what a host needs.
  LOG(INFO) << "avbase.pipeline: audio device '" << device_id
            << "' default=" << is_default << " "
            << OutputDeviceStatusToString(status);
}

base::scoped_refptr<base::SequencedTaskRunner>
PipelineImpl::GetOverlayTaskRunner() {
  // No overlay sequence exists in this build; the renderer must avoid the
  // work rather than run it inline (RendererClient's documented contract).
  return nullptr;
}

MediaInfo PipelineImpl::media_info() const {
  base::AutoLock scoped(snapshot_lock_);
  return media_info_;
}

}  // namespace avbase::media
