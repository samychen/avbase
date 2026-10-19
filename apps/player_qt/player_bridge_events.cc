// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// PlayerBridge's event half: the hop from avbase's dispatch sequence to the GUI
// thread, and what each event means for the properties QML binds to.
//
// Split out of player_bridge.cc, the way media/filters separates
// renderer_impl.cc from renderer_impl_reporting.cc: the file that owns the SDK
// calls and the file that owns the event vocabulary are different concerns.
// The immediate reason is that player_bridge.cc had reached the project's
// 500-line ceiling (C1) with real work, not with filler.

#include "apps/player_qt/player_bridge.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QSize>
#include <QString>

#include "apps/player_qt/timeline_model.h"
#include "media/base/media_info.h"

namespace avbase::apps {

namespace {

QString StateName(PlayerState state) {
  return QString::fromLatin1(GetPlayerStateName(state));
}

}  // namespace

void PlayerBridge::OnEvent(const PlayerEvent& event) {
  // Runs on avbase's event dispatch sequence. Touching a QObject member from
  // here would be a data race with the GUI thread, and calling into QML from a
  // non-GUI thread is not allowed at all, so the only thing this does is hand a
  // copy to the GUI thread. The copy is deliberate: |event| is a reference into
  // the dispatcher's storage and is dead once this returns.
  const PlayerEvent copy = event;
  QMetaObject::invokeMethod(
      this, [this, copy]() { ApplyEvent(copy); }, Qt::QueuedConnection);
}

void PlayerBridge::ApplyEvent(const PlayerEvent& event) {
  switch (event.type) {
  case EventType::kPrepared:
    if (const auto* payload = AsPrepared(event)) {
      const MediaInfo& info = payload->media_info;
      duration_ = info.duration.InSecondsF();
      live_ = info.is_live;
      emit durationChanged();
      emit liveChanged();
      RebuildTracks();
      // The panel describes the file that is open, so this is the first moment
      // it can be filled: before MediaInfo there is no duration to put on the
      // ruler. It used to be filled with hard-coded clip names at startup,
      // which is what made a freshly opened file show a project that did not
      // exist -- see the header of timeline_model.h.
      if (timeline_ != nullptr) {
        timeline_->loadFromMedia(
            QFileInfo(source_).fileName(), duration_,
            info.FirstStreamOfKind(media::StreamKind::kVideo) >= 0,
            info.FirstStreamOfKind(media::StreamKind::kAudio) >= 0);
      }
      if (const media::Size size = player_->video_natural_size();
          size.width > 0) {
        video_size_ = QSize(size.width, size.height);
        emit videoSizeChanged();
      }
      // The bridge set start_on_prepared=false so that this decision lives
      // in the UI rather than inside Prepare -- and the decision a player
      // makes for the file the user just opened is "play it". Pausing here
      // instead would leave the big play button showing a play glyph on a file
      // that has already buffered, which reads as a hang.
      if (player_) {
        player_->Start();
      }
    }
    break;

  case EventType::kStateChanged:
    if (const auto* payload = AsStateChanged(event)) {
      state_name_ = StateName(payload->to);
      emit stateNameChanged();
      SetPlaying(payload->to == PlayerState::kStarted);
    }
    break;

  case EventType::kVideoSizeChanged:
    if (const auto* payload = AsVideoSizeChanged(event)) {
      video_size_ =
          QSize(payload->natural_size.width, payload->natural_size.height);
      emit videoSizeChanged();
    }
    break;

  case EventType::kBufferingProgress:
    if (const auto* payload = AsBufferingProgress(event)) {
      buffered_ = payload->buffered_ahead.InSecondsF();
      emit bufferedChanged();
    }
    break;

  case EventType::kSeekCompleted:
    if (const auto* payload = AsSeekCompleted(event)) {
      position_ = payload->actual.InSecondsF();
      emit positionChanged();
    }
    break;

  case EventType::kTrackChanged:
    RebuildTracks();
    break;

  case EventType::kStats:
    if (const auto* payload = AsStats(event)) {
      stats_ = QString::fromStdString(payload->stats.ToJson());
      emit statsChanged();
    }
    break;

  case EventType::kCompleted:
    SetPlaying(false);
    position_ = duration_;
    emit positionChanged();
    break;

  case EventType::kError:
    if (const auto* payload = AsError(event)) {
      error_string_ = QString::fromStdString(payload->error.ToString());
      emit errorStringChanged();
    }
    SetPlaying(false);
    break;

  default:
    break;
  }
}

}  // namespace avbase::apps
