// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/player_qt/player_bridge.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include <QDir>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include "apps/player_qt/frame_mailbox.h"
#include "apps/player_qt/qt_video_sink.h"
#include "apps/player_qt/timeline_model.h"
#include "base/functional/bind.h"
#include "media/base/media_info.h"

#if defined(AVBASE_QT_HAVE_SDL2_AUDIO)
#include "platform/sdl2/sdl2_audio_sink.h"
#endif

namespace avbase::apps {

namespace {

// The poll cadence for the properties QML binds to a scrubber. The Player's
// query methods are lock-free snapshots, so this costs a few atomics; the
// event stream carries the same information far less often.
constexpr int kPollIntervalMs = 100;

QVariantMap StreamToVariant(const media::StreamInfo& stream) {
  QVariantMap map;
  map.insert(QStringLiteral("index"), stream.index);
  map.insert(QStringLiteral("codec"),
             QString::fromStdString(stream.codec_name));
  map.insert(QStringLiteral("language"),
             QString::fromStdString(stream.language));

  // A label the user can choose between. A codec name is not enough: a file
  // with two AAC tracks needs "English" and "Commentary", so this is title,
  // then language, then a positional fallback.
  QString label;
  if (!stream.title.empty()) {
    label = QString::fromStdString(stream.title);
  } else if (!stream.language.empty()) {
    label = QString::fromStdString(stream.language);
  } else {
    label = QStringLiteral("Track %1").arg(stream.index);
  }
  if (stream.kind == media::StreamKind::kVideo) {
    label += QStringLiteral("  %1x%2 %3")
                 .arg(stream.natural_size.width)
                 .arg(stream.natural_size.height)
                 .arg(QString::fromStdString(stream.codec_name));
  } else if (stream.kind == media::StreamKind::kAudio) {
    label += QStringLiteral("  %1ch %2Hz")
                 .arg(stream.channels)
                 .arg(stream.sample_rate);
  }
  map.insert(QStringLiteral("label"), label);
  map.insert(QStringLiteral("isDefault"), stream.is_default);
  return map;
}

}  // namespace

PlayerBridge::PlayerBridge(TimelineModel* timeline, QObject* parent)
    : QObject(parent), timeline_(timeline) {}

PlayerBridge::~PlayerBridge() {
  // Order matters. Dropping the handler first means the Player can no longer
  // queue an event against an object that is halfway through destruction;
  // ~Player() then joins its event and pipeline threads while this object is
  // still alive, which is the only window in which that join is safe.
  if (player_) {
    player_->SetEventHandler(Player::EventHandler());
    player_->StopSync(base::Seconds(3));
    player_.reset();
  }
}

void PlayerBridge::setMailbox(FrameMailbox* mailbox) {
  if (player_ || mailbox == nullptr) {
    return;
  }
  mailbox_ = mailbox;

  PlayerConfig config;
  // The UI decides when playback starts: an autostart hidden inside Prepare
  // would make the big play button a lie, and its state machine rejects the
  // Start() a host writes right after PrepareSync() for that reason.
  config.start_on_prepared = false;

  auto deps = std::make_unique<Deps>();
  // Two isolation switches, for bisecting a crash between the three components
  // that can cause one (decoder, our sink, the audio device). Unset in normal
  // use; each replaces its factory with avbase's null implementation.
  if (!qEnvironmentVariableIsSet("AVBASE_QT_NULL_VIDEO")) {
    deps->video_sink_factory = std::make_shared<QtVideoSinkFactory>(mailbox);
  }
#if defined(AVBASE_QT_HAVE_SDL2_AUDIO)
  if (!qEnvironmentVariableIsSet("AVBASE_QT_NULL_AUDIO")) {
    // Real sound. Sdl2AudioSink lazily runs SDL_INIT_AUDIO/QUIT itself, so the
    // app does not have to start the SDL audio subsystem -- and it needs no SDL
    // window, which is the point: Qt owns the window here.
    deps->audio_sink_factory = std::make_shared<media::Sdl2AudioSinkFactory>();
  }
#endif

  player_ = std::make_unique<Player>(config, std::move(deps));
  player_->SetEventHandler(
      base::BindRepeating(&PlayerBridge::OnEvent, base::Unretained(this)));
  // "Render nowhere, keep audio": the QtVideoSink presents by handing frames to
  // the mailbox, so there is no window for the surface argument to name.
  player_->SetVideoSurface(media::NativeDisplay::None());

  poll_ = new QTimer(this);
  poll_->setInterval(kPollIntervalMs);
  QObject::connect(poll_, &QTimer::timeout, this, &PlayerBridge::PollQueries);
  poll_->start();
}

void PlayerBridge::open(const QString& url) {
  if (!player_) {
    error_string_ = QStringLiteral("player not initialised");
    emit errorStringChanged();
    return;
  }
  QString local = url;
  const QUrl parsed(url);
  if (parsed.isLocalFile()) {
    local = parsed.toLocalFile();
  }
  if (local.trimmed().isEmpty()) {
    return;
  }

  // A second open() from kStarted is illegal in the state machine, so return to
  // kIdle first. This is the same sequence a playlist uses.
  if (player_->state() != PlayerState::kIdle &&
      player_->state() != PlayerState::kInitialized) {
    player_->StopSync(base::Seconds(3));
    player_->Reset();
  }

  error_string_.clear();
  emit errorStringChanged();
  source_ = local;
  emit sourceChanged();
  // The previous file's lanes go now, before the new one is known: a Prepare
  // that then fails leaves an empty panel instead of the last file's tracks,
  // and an empty panel is the honest answer to "what is in this file?".
  if (timeline_ != nullptr) {
    timeline_->clear();
  }

  const Status set = player_->SetDataSource(local.toStdString());
  if (!set) {
    error_string_ = QString::fromStdString(set.error().ToString());
    emit errorStringChanged();
    return;
  }
  const Status prepared = player_->PrepareAsync();
  if (!prepared) {
    error_string_ = QString::fromStdString(prepared.error().ToString());
    emit errorStringChanged();
  }
}

void PlayerBridge::playPause() {
  if (!player_) {
    return;
  }
  switch (player_->state()) {
  case PlayerState::kStarted:
    player_->Pause();
    break;
  case PlayerState::kPaused:
  case PlayerState::kPrepared:
    player_->Start();
    break;
  case PlayerState::kCompleted:
    // Replays from the beginning; the state machine documents this.
    player_->Start();
    break;
  default:
    break;
  }
}

void PlayerBridge::stop() {
  if (player_) {
    player_->Stop();
  }
}

void PlayerBridge::seekPreview(double seconds) {
  if (!player_) {
    return;
  }
  // Fire-and-forget, nearest keyframe: a drag emits dozens of these and only
  // the last one matters.
  player_->SeekTo(base::SecondsD(seconds));
}

void PlayerBridge::seekCommit(double seconds) {
  if (!player_) {
    return;
  }
  const auto result = player_->SeekTo(base::SecondsD(seconds),
                                      SeekMode::kAccurate, Player::SeekCB());
  if (!result) {
    error_string_ = QString::fromStdString(result.error().ToString());
    emit errorStringChanged();
  }
}

void PlayerBridge::stepOnce() {
  if (player_) {
    player_->StepOnce();
  }
}

void PlayerBridge::setVolume(double value) {
  if (!player_) {
    return;
  }
  const double clamped = std::clamp(value, 0.0, 1.0);
  player_->SetVolume(clamped);
  if (!qFuzzyCompare(clamped, volume_)) {
    volume_ = clamped;
    emit volumeChanged();
  }
}

void PlayerBridge::toggleMute() {
  if (!player_) {
    return;
  }
  const bool next = !muted_;
  player_->SetMuted(next);
  muted_ = next;
  emit mutedChanged();
}

void PlayerBridge::setRate(double value) {
  if (!player_) {
    return;
  }
  player_->SetPlaybackRate(value);  // The SDK clamps to [0.25, 4.0].
  const double applied = player_->playback_rate();
  if (!qFuzzyCompare(applied, rate_)) {
    rate_ = applied;
    emit rateChanged();
  }
}

void PlayerBridge::selectAudioTrack(int index) {
  if (!player_) {
    return;
  }
  if (const Status s =
          player_->SelectTrack(media::DemuxerStreamType::kAudio, index);
      !s) {
    error_string_ = QString::fromStdString(s.error().ToString());
    emit errorStringChanged();
    return;
  }
  audio_track_ = index;
  emit tracksChanged();
}

void PlayerBridge::selectVideoTrack(int index) {
  if (!player_) {
    return;
  }
  if (const Status s =
          player_->SelectTrack(media::DemuxerStreamType::kVideo, index);
      !s) {
    error_string_ = QString::fromStdString(s.error().ToString());
    emit errorStringChanged();
    return;
  }
  video_track_ = index;
  emit tracksChanged();
}

void PlayerBridge::snapshot(const QString& path) {
  if (!player_) {
    return;
  }
  const Status s =
      player_->TakeSnapshot(player_->GetMediaTime(), path.toStdString());
  if (!s) {
    error_string_ = QString::fromStdString(s.error().ToString());
    emit errorStringChanged();
  }
}

QString PlayerBridge::diagnostics() const {
  return player_ ? QString::fromStdString(player_->DumpDiagnostics())
                 : QStringLiteral("{}");
}

QString PlayerBridge::home_path() const {
  return QDir::homePath();
}

QString PlayerBridge::formatTime(double seconds) const {
  if (!std::isfinite(seconds) || seconds < 0.0) {
    seconds = 0.0;
  }
  const auto total = static_cast<int64_t>(seconds);
  const int64_t hours = total / 3600;
  const int64_t minutes = (total % 3600) / 60;
  const int64_t secs = total % 60;
  if (hours > 0) {
    return QStringLiteral("%1:%2:%3")
        .arg(hours)
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
  }
  return QStringLiteral("%1:%2")
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(secs, 2, 10, QLatin1Char('0'));
}

void PlayerBridge::PollQueries() {
  if (!player_) {
    return;
  }
  const double pos = player_->GetMediaTime().InSecondsF();
  if (std::abs(pos - position_) > 0.001) {
    position_ = pos;
    emit positionChanged();
  }
  const double buf = player_->GetBufferedTime().InSecondsF();
  if (std::abs(buf - buffered_) > 0.001) {
    buffered_ = buf;
    emit bufferedChanged();
  }
  SetPlaying(player_->IsPlaying());

  if (timeline_ != nullptr && !player_->is_live()) {
    timeline_->setPlayhead(pos);
  }
}

void PlayerBridge::SetPlaying(bool playing) {
  if (playing == playing_) {
    return;
  }
  playing_ = playing;
  emit playingChanged();
}

void PlayerBridge::RebuildTracks() {
  audio_tracks_.clear();
  video_tracks_.clear();
  text_tracks_.clear();
  audio_track_ = -1;
  video_track_ = -1;

  const std::optional<MediaInfo> info =
      player_ ? player_->media_info() : std::nullopt;
  if (!info) {
    emit tracksChanged();
    return;
  }
  for (const media::StreamInfo& stream : info->streams) {
    switch (stream.kind) {
    case media::StreamKind::kAudio:
      audio_tracks_.append(StreamToVariant(stream));
      if (audio_track_ < 0) {
        audio_track_ = stream.index;
      }
      break;
    case media::StreamKind::kVideo:
      video_tracks_.append(StreamToVariant(stream));
      if (video_track_ < 0) {
        video_track_ = stream.index;
      }
      break;
    case media::StreamKind::kText:
      text_tracks_.append(StreamToVariant(stream));
      break;
    default:
      break;
    }
  }
  emit tracksChanged();
}

}  // namespace avbase::apps
