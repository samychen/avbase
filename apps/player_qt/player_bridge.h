// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The only place avbase and Qt meet.
//
// avbase's Player is a C++ SDK with an event handler that runs on its own
// dispatch sequence; QML wants bindable properties that change on the GUI
// thread. Everything awkward about embedding lives in this one class, so both
// sides stay idiomatic: the Player is used exactly as its header documents, and
// the QML never learns that the frame clock is not a QTimer.
//
// THREADING RULES this class exists to enforce:
//   1. The Player is CONSTRUCTED and DESTROYED on the GUI thread. ~Player()
//      joins the event thread, and doing that from a thread QML does not own is
//      how shutdown turns into a hang.
//   2. OnEvent() runs on avbase's event dispatch thread and touches NOTHING but
//      a queued invocation. Reading a Q_PROPERTY from there would be a data
//      race, and calling into QML from there is not allowed at all.
//   3. The poll timer runs on the GUI thread and only calls the Player's
//      lock-free query methods (GetMediaTime, GetBufferedTime, ...).

#ifndef AVBASE_APPS_PLAYER_QT_PLAYER_BRIDGE_H_
#define AVBASE_APPS_PLAYER_QT_PLAYER_BRIDGE_H_

#include <QObject>
#include <QSize>
#include <QString>
#include <QVariantList>

#include <memory>

#include "base/time/time.h"
#include "player/public/player.h"
#include "player/public/player_event.h"

class QTimer;

namespace avbase::apps {

class FrameMailbox;
class TimelineModel;

class PlayerBridge : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString source READ source NOTIFY sourceChanged)
  Q_PROPERTY(double position READ position NOTIFY positionChanged)
  Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
  Q_PROPERTY(double buffered READ buffered NOTIFY bufferedChanged)
  Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
  Q_PROPERTY(bool live READ live NOTIFY liveChanged)
  Q_PROPERTY(QString stateName READ state_name NOTIFY stateNameChanged)
  Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
  Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
  Q_PROPERTY(double rate READ rate WRITE setRate NOTIFY rateChanged)
  Q_PROPERTY(QVariantList audioTracks READ audio_tracks NOTIFY tracksChanged)
  Q_PROPERTY(QVariantList videoTracks READ video_tracks NOTIFY tracksChanged)
  Q_PROPERTY(QVariantList textTracks READ text_tracks NOTIFY tracksChanged)
  Q_PROPERTY(int audioTrack READ audio_track NOTIFY tracksChanged)
  Q_PROPERTY(int videoTrack READ video_track NOTIFY tracksChanged)
  Q_PROPERTY(QString stats READ stats NOTIFY statsChanged)
  Q_PROPERTY(QSize videoSize READ video_size NOTIFY videoSizeChanged)
  Q_PROPERTY(QString errorString READ error_string NOTIFY errorStringChanged)
  // Where the snapshot menu item writes. Exposed rather than hard-coded in QML
  // so a host can point it at a project directory later.
  Q_PROPERTY(QString homePath READ home_path CONSTANT)

 public:
  // |timeline| is referenced but not owned: selecting a clip on the timeline
  // moves the player's clock, so the two need to see each other. It may be
  // null.
  explicit PlayerBridge(TimelineModel* timeline, QObject* parent = nullptr);
  ~PlayerBridge() override;

  // Must be called before open(); the sink factory captures the pointer at
  // construction, so it cannot be swapped afterwards.
  void setMailbox(FrameMailbox* mailbox);

  QString source() const { return source_; }
  double position() const { return position_; }
  double duration() const { return duration_; }
  double buffered() const { return buffered_; }
  bool playing() const { return playing_; }
  bool live() const { return live_; }
  QString state_name() const { return state_name_; }
  double volume() const { return volume_; }
  bool muted() const { return muted_; }
  double rate() const { return rate_; }
  QVariantList audio_tracks() const { return audio_tracks_; }
  QVariantList video_tracks() const { return video_tracks_; }
  QVariantList text_tracks() const { return text_tracks_; }
  int audio_track() const { return audio_track_; }
  int video_track() const { return video_track_; }
  QString stats() const { return stats_; }
  QSize video_size() const { return video_size_; }
  QString error_string() const { return error_string_; }
  QString home_path() const;

  // ---- Commands, all callable from QML ------------------------------------
  Q_INVOKABLE void open(const QString& url);
  Q_INVOKABLE void playPause();
  Q_INVOKABLE void stop();
  // Seeking while the scrubber is dragged: fast, keyframe-anchored, and
  // intentionally not queued with a callback -- a drag produces dozens of these
  // and only the last one matters.
  Q_INVOKABLE void seekPreview(double seconds);
  // Seeking on release: lands on the exact frame (SeekMode::kAccurate).
  Q_INVOKABLE void seekCommit(double seconds);
  Q_INVOKABLE void stepOnce();
  Q_INVOKABLE void setVolume(double value);
  Q_INVOKABLE void toggleMute();
  Q_INVOKABLE void setRate(double value);
  Q_INVOKABLE void selectAudioTrack(int index);
  Q_INVOKABLE void selectVideoTrack(int index);
  Q_INVOKABLE void snapshot(const QString& path);
  Q_INVOKABLE QString diagnostics() const;
  Q_INVOKABLE QString formatTime(double seconds) const;

 signals:
  void sourceChanged();
  void positionChanged();
  void durationChanged();
  void bufferedChanged();
  void playingChanged();
  void liveChanged();
  void stateNameChanged();
  void volumeChanged();
  void mutedChanged();
  void rateChanged();
  void tracksChanged();
  void statsChanged();
  void videoSizeChanged();
  void errorStringChanged();

 private:
  // Event dispatch thread. Must only queue.
  void OnEvent(const PlayerEvent& event);
  // GUI thread. Applies a queued event.
  void ApplyEvent(const PlayerEvent& event);
  // GUI thread, from the poll timer: the cheap queries.
  void PollQueries();
  void RebuildTracks();
  void SetPlaying(bool playing);

  TimelineModel* timeline_;
  FrameMailbox* mailbox_{nullptr};
  std::unique_ptr<Player> player_;
  QTimer* poll_{nullptr};

  QString source_;
  double position_{0.0};
  double duration_{0.0};
  double buffered_{0.0};
  bool playing_{false};
  bool live_{false};
  QString state_name_{QStringLiteral("idle")};
  double volume_{1.0};
  bool muted_{false};
  double rate_{1.0};
  QVariantList audio_tracks_;
  QVariantList video_tracks_;
  QVariantList text_tracks_;
  int audio_track_{-1};
  int video_track_{-1};
  QString stats_;
  QSize video_size_;
  QString error_string_;
};

}  // namespace avbase::apps

#endif  // AVBASE_APPS_PLAYER_QT_PLAYER_BRIDGE_H_
