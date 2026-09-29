// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/player_event.h"

namespace ijkpp {

const char* GetPlayerStateName(PlayerState state) {
  switch (state) {
    case PlayerState::kIdle:       return "Idle";
    case PlayerState::kInitialized:return "Initialized";
    case PlayerState::kPreparing:  return "Preparing";
    case PlayerState::kPrepared:   return "Prepared";
    case PlayerState::kStarted:    return "Started";
    case PlayerState::kPaused:     return "Paused";
    case PlayerState::kBuffering:  return "Buffering";
    case PlayerState::kStopping:   return "Stopping";
    case PlayerState::kStopped:    return "Stopped";
    case PlayerState::kCompleted:  return "Completed";
    case PlayerState::kError:      return "Error";
    case PlayerState::kEnd:        return "End";
  }
  return "Invalid";
}

const char* GetEventTypeName(EventType type) {
  switch (type) {
    case EventType::kPrepared: return "Prepared";
    case EventType::kCompleted: return "Completed";
    case EventType::kError: return "Error";
    case EventType::kFlushed: return "Flushed";
    case EventType::kStateChanged: return "StateChanged";
    case EventType::kVideoSizeChanged: return "VideoSizeChanged";
    case EventType::kRotationChanged: return "RotationChanged";
    case EventType::kRenderingStarted: return "RenderingStarted";
    case EventType::kDecodingStarted: return "DecodingStarted";
    case EventType::kDecoderOpened: return "DecoderOpened";
    case EventType::kDecoderFallback: return "DecoderFallback";
    case EventType::kStageReached: return "StageReached";
    case EventType::kWaiting: return "Waiting";
    case EventType::kBufferingStarted: return "BufferingStarted";
    case EventType::kBufferingEnded: return "BufferingEnded";
    case EventType::kBufferingProgress: return "BufferingProgress";
    case EventType::kBufferedBytesUpdate: return "BufferedBytesUpdate";
    case EventType::kBufferedTimeUpdate: return "BufferedTimeUpdate";
    case EventType::kSeekCompleted: return "SeekCompleted";
    case EventType::kAccurateSeekCompleted: return "AccurateSeekCompleted";
    case EventType::kSeekRenderingStarted: return "SeekRenderingStarted";
    case EventType::kTimedText: return "TimedText";
    case EventType::kTrackChanged: return "TrackChanged";
    case EventType::kSnapshotCompleted: return "SnapshotCompleted";
    case EventType::kStats: return "Stats";
    case EventType::kHdrUnsupported: return "HdrUnsupported";
  }
  return "Invalid";
}

namespace {

template <typename T>
const T* PayloadIf(const PlayerEvent& event, EventType expected) {
  if (event.type != expected) {
    return nullptr;
  }
  return std::get_if<T>(&event.payload);
}

}  // namespace

#define IJKPP_DEFINE_EVENT_ACCESSOR(Name, Type, Kind)                     \
  const Type* Name(const PlayerEvent& event) {                            \
    return PayloadIf<Type>(event, EventType::Kind);                       \
  }

IJKPP_DEFINE_EVENT_ACCESSOR(AsPrepared, PreparedPayload, kPrepared)
IJKPP_DEFINE_EVENT_ACCESSOR(AsError, ErrorPayload, kError)
IJKPP_DEFINE_EVENT_ACCESSOR(AsStateChanged, StateChangedPayload, kStateChanged)
IJKPP_DEFINE_EVENT_ACCESSOR(AsVideoSizeChanged, VideoSizeChangedPayload, kVideoSizeChanged)
IJKPP_DEFINE_EVENT_ACCESSOR(AsRotationChanged, RotationChangedPayload, kRotationChanged)
IJKPP_DEFINE_EVENT_ACCESSOR(AsRenderingStarted, RenderingStartedPayload, kRenderingStarted)
IJKPP_DEFINE_EVENT_ACCESSOR(AsDecodingStarted, DecodingStartedPayload, kDecodingStarted)
IJKPP_DEFINE_EVENT_ACCESSOR(AsStageReached, StageReachedPayload, kStageReached)
IJKPP_DEFINE_EVENT_ACCESSOR(AsBufferingProgress, BufferingProgressPayload, kBufferingProgress)
IJKPP_DEFINE_EVENT_ACCESSOR(AsBufferedUpdate, BufferedUpdatePayload, kBufferedTimeUpdate)
IJKPP_DEFINE_EVENT_ACCESSOR(AsSeekCompleted, SeekCompletedPayload, kSeekCompleted)
IJKPP_DEFINE_EVENT_ACCESSOR(AsAccurateSeekCompleted, AccurateSeekCompletedPayload, kAccurateSeekCompleted)
IJKPP_DEFINE_EVENT_ACCESSOR(AsTimedText, TimedTextPayload, kTimedText)
IJKPP_DEFINE_EVENT_ACCESSOR(AsTrackChanged, TrackChangedPayload, kTrackChanged)
IJKPP_DEFINE_EVENT_ACCESSOR(AsSnapshotCompleted, SnapshotCompletedPayload, kSnapshotCompleted)
IJKPP_DEFINE_EVENT_ACCESSOR(AsStats, StatsPayload, kStats)
#undef IJKPP_DEFINE_EVENT_ACCESSOR

bool ToLegacyEvent(const PlayerEvent& event, LegacyEvent* out) {
  if (!out) {
    return false;
  }
  out->what = 0;
  out->arg1 = 0;
  out->arg2 = 0;
  out->obj.clear();

  switch (event.type) {
    case EventType::kFlushed:            out->what = 0;   return true;
    case EventType::kPrepared:           out->what = 200; return true;
    case EventType::kCompleted:          out->what = 300; return true;
    case EventType::kError:
      if (const auto* p = AsError(event)) {
        out->what = 100;
        out->arg1 = static_cast<int64_t>(p->error.code());
        out->obj = p->error.ToString();
        return true;
      }
      return false;
    case EventType::kVideoSizeChanged:
      if (const auto* p = AsVideoSizeChanged(event)) {
        out->what = 400;
        out->arg1 = p->natural_size.width;
        out->arg2 = p->natural_size.height;
        return true;
      }
      return false;
    case EventType::kRotationChanged:
      if (const auto* p = AsRotationChanged(event)) {
        out->what = 404;
        out->arg1 = p->degrees;
        return true;
      }
      return false;
    case EventType::kRenderingStarted:
      if (const auto* p = AsRenderingStarted(event)) {
        out->what = p->type == media::DemuxerStreamType::kVideo ? 402 : 403;
        return true;
      }
      return false;
    case EventType::kDecodingStarted:
      if (const auto* p = AsDecodingStarted(event)) {
        out->what = p->type == media::DemuxerStreamType::kVideo ? 406 : 405;
        return true;
      }
      return false;
    case EventType::kStageReached:
      if (const auto* p = AsStageReached(event)) {
        using S = StageReachedPayload::Stage;
        out->what = p->stage == S::kOpenInput ? 407
                  : p->stage == S::kFindStreamInfo ? 408 : 409;
        return true;
      }
      return false;
    case EventType::kBufferingStarted:   out->what = 500; return true;
    case EventType::kBufferingEnded:     out->what = 501; return true;
    case EventType::kBufferingProgress:
      if (const auto* p = AsBufferingProgress(event)) {
        out->what = 502;
        out->arg1 = p->buffered_ahead.InMilliseconds();
        out->arg2 = p->percent;
        return true;
      }
      return false;
    case EventType::kSeekCompleted:
      if (const auto* p = AsSeekCompleted(event)) {
        out->what = 600;
        out->arg1 = p->actual.InMilliseconds();
        out->arg2 = static_cast<int64_t>(p->result.code());
        return true;
      }
      return false;
    case EventType::kStateChanged:       out->what = 700; return true;
    case EventType::kTimedText:
      if (const auto* p = AsTimedText(event)) {
        out->what = 800;
        out->obj = p->text;
        return true;
      }
      return false;
    case EventType::kAccurateSeekCompleted:
      if (const auto* p = AsAccurateSeekCompleted(event)) {
        out->what = 900;
        out->arg1 = p->position.InMilliseconds();
        return true;
      }
      return false;
    case EventType::kSnapshotCompleted:
      if (const auto* p = AsSnapshotCompleted(event)) {
        out->what = 1000;
        out->arg1 = p->at.InMilliseconds();
        out->arg2 = static_cast<int64_t>(p->result.code());
        out->obj = p->file_path;
        return true;
      }
      return false;
    case EventType::kDecoderOpened:      out->what = 10001; return true;
    // New in ijkpp; no ijkplayer equivalent.
    case EventType::kDecoderFallback:
    case EventType::kWaiting:
    case EventType::kBufferedBytesUpdate:
    case EventType::kBufferedTimeUpdate:
    case EventType::kSeekRenderingStarted:
    case EventType::kTrackChanged:
    case EventType::kStats:
    case EventType::kHdrUnsupported:
      return false;
  }
  return false;
}

}  // namespace ijkpp
