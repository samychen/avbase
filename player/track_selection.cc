// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Runtime track selection (Phase 4). The only part of PlayerImpl that maps a
// container stream index onto a live DemuxerStream handover; kept in its own
// file because both player_impl.cc and player_impl_events.cc sit at the
// C1 line-limit ceiling and this path grows independently of both.

#include "player/player_impl.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "media/base/media_info.h"
#include "media/base/pipeline_status.h"

namespace avbase {

Status PlayerImpl::SelectTrack(media::DemuxerStreamType type,
                               int stream_index) {
  PlayerState current;
  {
    base::AutoLock scoped(state_lock_);
    current = machine_.state();
  }
  if (current != PlayerState::kStarted && current != PlayerState::kPaused &&
      current != PlayerState::kPrepared) {
    return base::unexpected(MediaError(
        ErrorCode::kInvalidState,
        std::string("SelectTrack called in state ") +
            GetPlayerStateName(current),
        "track switching hands over the sub-renderer, which needs a "
        "prepared or playing pipeline",
        "wait for kPrepared before switching tracks"));
  }

  const MediaInfo info = pipeline_ ? pipeline_->media_info() : MediaInfo();
  const StreamInfo* stream_info = info.FindStream(stream_index);
  const StreamKind wanted_kind = type == media::DemuxerStreamType::kAudio
                                     ? StreamKind::kAudio
                                     : StreamKind::kUnknown;
  if (!stream_info || stream_info->kind != wanted_kind) {
    return base::unexpected(MediaError(
        ErrorCode::kInvalidArgument, "no such track",
        "media_info().streams has no stream " + std::to_string(stream_index) +
            " of the requested type",
        "pick an index from media_info().streams; StreamsOfKind(kAudio) "
        "lists the audio tracks with their index, language and title"));
  }
  if (type != media::DemuxerStreamType::kAudio) {
    // kText has no TextRenderer (RendererImpl gap 3); kVideo needs the
    // video-side handover, which lands after the audio one proves the
    // mechanism.
    return base::unexpected(MediaError(
        ErrorCode::kNotImplemented,
        type == media::DemuxerStreamType::kText
            ? "subtitle track selection is not supported yet"
            : "video track selection is not supported yet",
        type == media::DemuxerStreamType::kText
            ? "this build has no TextRenderer; the base exposes text as "
              "events only once the text leg lands"
            : "the video-side renderer handover is not wired yet",
        "for subtitles, an external renderer can consume the text stream; "
        "for video, restart playback with video.selected_stream set"));
  }

  const int old_index = audio_track_index_.exchange(stream_index);
  pipeline_->SelectAudioTrack(
      stream_index,
      base::BindOnce(
          [](EventHub* hub, media::DemuxerStreamType type, int old_index,
             int new_index, media::PipelineStatus status) {
            if (status != media::PipelineStatus::kOk) {
              // The renderer already reported the actionable reason through
              // Client::OnError; nothing further to add here.
              return;
            }
            TrackChangedPayload payload;
            payload.type = type;
            payload.old_index = old_index;
            payload.new_index = new_index;
            hub->Post(EventType::kTrackChanged, std::move(payload),
                      media::kNoTimestamp);
          },
          &event_hub_, type, old_index, stream_index));
  return Status();
}

}  // namespace avbase
