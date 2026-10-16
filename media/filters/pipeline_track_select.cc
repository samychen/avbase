// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Runtime track selection (Phase 4.2), extracted from pipeline_impl.cc for
// the same reason player/track_selection.cc exists: both hosts sit at the
// C1 ceiling and this path grows independently.

#include "media/filters/pipeline_impl.h"

#include <vector>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"

namespace avbase::media {

void PipelineImpl::SelectAudioTrack(int stream_index,
                                    PipelineStatusCallback cb) {
  SelectTrack(DemuxerStreamType::kAudio, stream_index, std::move(cb));
}

void PipelineImpl::SelectTextTrack(int stream_index,
                                   PipelineStatusCallback cb) {
  SelectTrack(DemuxerStreamType::kText, stream_index, std::move(cb));
}

void PipelineImpl::SelectVideoTrack(int stream_index,
                                    PipelineStatusCallback cb) {
  SelectTrack(DemuxerStreamType::kVideo, stream_index, std::move(cb));
}

void PipelineImpl::SelectTrack(DemuxerStreamType type, int stream_index,
                               PipelineStatusCallback cb) {
  if (!media_runner_) {
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
    return;
  }
  // Bound through the factory, not base::Unretained(this): this is a
  // cross-sequence hop posted through media_runner_, so the caller may destroy
  // the Player (and this object) the instant SelectTrack returns while the
  // post is already in flight. Every sibling hop in pipeline_impl.cc binds
  // weak_factory_.GetWeakPtr() for exactly this reason (pipeline_impl.h:237-243
  // gives the full account); this one was the last Unretained left.
  media_runner_->PostTask(FROM_HERE,
                          base::BindOnce(&PipelineImpl::DoSelectTrack,
                                         weak_factory_.GetWeakPtr(), type,
                                         stream_index, std::move(cb)));
}

void PipelineImpl::DoSelectTrack(DemuxerStreamType type, int stream_index,
                                 PipelineStatusCallback cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kReady || !renderer_ || !composite_) {
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
    return;
  }
  // Asked of the resource rather than answered here: an aggregate source's
  // indices are per source (docs/14 stage 4 -- two children both count their
  // streams from their own container order), so "the stream whose index is N"
  // has no answer a comparison in THIS loop could reach. The loop used to read
  // `composite_->GetStreams(type)`, which answers SOURCE 0 alone: a secondary
  // source's subtitle track was not merely unselectable, selecting its index
  // silently matched nothing and returned kTrackSwitchError.
  DemuxerStream* target = composite_->FindStream(type, stream_index);
  if (!target) {
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
    return;
  }
  // Which source this index belongs to, derived from the same aggregate
  // numbering FindStream used, so the two never disagree about the source
  // (docs/14 stage 4).
  const int source_id = composite_->SourceIdForIndex(type, stream_index);
  // Video and audio legs are source 0's alone (docs/14 §4.2: the video switch
  // hands over source 0's renderer, sink and clock hook; the audio leg is
  // source 0 only -- secondary audio is seen but not selected). Stage 4's
  // aggregate numbering made a secondary source's video/audio index
  // RESOLVABLE, so the refusal must happen BEFORE the routing below:
  // SetActiveStream would re-point that child's active stream, the renderer
  // would then refuse the handover with nothing to roll back, and the stream
  // the leg still reads would go quiet -- "the track switched and nothing
  // arrived", reported as kOk. The text leg is per-source by design (stage
  // 4), which is why only video/audio are bounded here.
  if (type != DemuxerStreamType::kText && source_id != 0) {
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
    return;
  }
  // Re-point routing BEFORE the handover: the moment the old consumer stops
  // draining its stream, that stream's queue would wedge the demux loop on
  // its own watermark and starve the very track we are switching to.
  composite_->SetActiveStream(type, stream_index);
  // The renderer reports a failed handover through Client::OnError and still
  // runs the completion closure; that closure only means "the attempt
  // finished and the pipeline is consistent".
  renderer_->OnTracksChanged(type, target,
                             base::BindOnce(
                                 [](PipelineStatusCallback cb) {
                                   std::move(cb).Run(PipelineStatus::kOk);
                                 },
                                 std::move(cb)),
                             source_id);
}
}  // namespace avbase::media
