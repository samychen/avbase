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

void PipelineImpl::SelectTrack(DemuxerStreamType type, int stream_index,
                               PipelineStatusCallback cb) {
  if (!media_runner_) {
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
    return;
  }
  media_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&PipelineImpl::DoSelectTrack, base::Unretained(this),
                     type, stream_index, std::move(cb)));
}

void PipelineImpl::DoSelectTrack(DemuxerStreamType type, int stream_index,
                                 PipelineStatusCallback cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kReady || !renderer_ || !demuxer_) {
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
    return;
  }
  DemuxerStream* target = nullptr;
  for (DemuxerStream* stream : demuxer_->GetStreams(type)) {
    if (stream->stream_index() == stream_index) {
      target = stream;
      break;
    }
  }
  if (!target) {
    std::move(cb).Run(PipelineStatus::kTrackSwitchError);
    return;
  }
  // Re-point routing BEFORE the handover: the moment the old consumer stops
  // draining its stream, that stream's queue would wedge the demux loop on
  // its own watermark and starve the very track we are switching to.
  demuxer_->SetActiveStream(type, stream_index);
  // The renderer reports a failed handover through Client::OnError and still
  // runs the completion closure; that closure only means "the attempt
  // finished and the pipeline is consistent".
  renderer_->OnTracksChanged(
      type, target,
      base::BindOnce(
          [](PipelineStatusCallback cb) {
            std::move(cb).Run(PipelineStatus::kOk);
          },
          std::move(cb)));
}
}  // namespace avbase::media
