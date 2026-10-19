// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The timeline data model behind the Shotcut-style panel.
//
// This is deliberately a MODEL and not a widget: every editor UI (QML grid,
// QWidget timeline, web canvas) needs the same four things -- rows of clips,
// each with a track index, a start and a length, plus a playhead -- and only
// the drawing differs. Keeping the arithmetic here is also what makes the panel
// testable without a screen.
//
// WHAT THIS MODEL IS NOT: a project. There is no project-file loader and no
// mixer behind it yet, so the clips it holds describe the file that is
// currently open and nothing else. It used to be filled with six hard-coded
// clip names
// ("intro.mp4", "b-roll.mp4", ...) so the skeleton had something to draw, which
// meant a freshly opened file showed a timeline belonging to a project that did
// not exist. A placeholder that looks like data is worse than an empty panel:
// the empty panel is obviously unfinished, while the fake one gets read.
//
// What a real editor adds on top (and what is NOT here): undo/redo, ripple
// edits, snapping, multi-clip mixing, and the project-file round trip.

#ifndef AVBASE_APPS_PLAYER_QT_TIMELINE_MODEL_H_
#define AVBASE_APPS_PLAYER_QT_TIMELINE_MODEL_H_

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QVariant>

namespace avbase::apps {

class TimelineModel : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
  Q_PROPERTY(
      double playhead READ playhead WRITE setPlayhead NOTIFY playheadChanged)
  Q_PROPERTY(double pixelsPerSecond READ pixels_per_second WRITE
                 setPixelsPerSecond NOTIFY pixelsPerSecondChanged)
  // Four lanes: V2, V1, A1, A2 -- fixed, because the QML header column draws
  // them by index and there is no project format that could ask for more.
  Q_PROPERTY(int trackCount READ track_count CONSTANT)

 public:
  enum Role {
    kNameRole = Qt::UserRole + 1,
    kTrackRole,
    kStartRole,
    kDurationRole,
    kKindRole,
    kColorRole,
  };

  struct Clip {
    QString name;
    int track{0};          // 0 is the topmost video track.
    double start{0.0};     // Seconds on the project timeline.
    double duration{0.0};  // Seconds.
    QString kind;          // "video" | "audio" | "image".
    QString color;
  };

  explicit TimelineModel(QObject* parent = nullptr);

  // QAbstractListModel:
  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index,
                int role = Qt::DisplayRole) const override;
  QHash<int, QByteArray> roleNames() const override;

  // The project's length in seconds: the media's own duration, floored so an
  // empty panel still has a ruler wide enough to be recognisable as one.
  //
  // Deliberately NOT derived from the clips. The playhead's clamp is measured
  // against this, and deriving it from the clips made the clamp a function of
  // the placeholder data -- which is how a 30 s file ended up with a playhead
  // that stopped moving at 18 s.
  double duration() const;
  double playhead() const { return playhead_; }
  double pixels_per_second() const { return pixels_per_second_; }
  int track_count() const { return track_count_; }

  void setPlayhead(double seconds);
  void setPixelsPerSecond(double pps);

  // Replaces the panel's contents with the tracks the media named |name|
  // actually carries. A source with no duration -- a live stream, or one the
  // container did not state -- clears the panel instead: a timeline that cannot
  // place a frame on a ruler has nothing honest to draw.
  Q_INVOKABLE void loadFromMedia(const QString& name, double duration,
                                 bool has_video, bool has_audio);
  Q_INVOKABLE void clear();

 signals:
  void durationChanged();
  void playheadChanged();
  void pixelsPerSecondChanged();

 private:
  // Appends without model notifications; every caller must already be inside a
  // beginResetModel()/endResetModel() pair.
  void AppendClip(const QString& name, int track, double start, double duration,
                  const QString& kind);

  QList<Clip> clips_;
  // The media's own duration, kept separately from the clips -- see duration().
  double project_duration_{0.0};
  double playhead_{0.0};
  double pixels_per_second_{60.0};
  const int track_count_{4};
};

}  // namespace avbase::apps

#endif  // AVBASE_APPS_PLAYER_QT_TIMELINE_MODEL_H_
