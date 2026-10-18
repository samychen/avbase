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
// What a real editor adds on top (and what is NOT here): undo/redo, ripple
// edits, snapping, and the project-file round trip. The clip struct is shaped
// so that adding those does not move any of them into the view.

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
  Q_PROPERTY(int trackCount READ track_count CONSTANT)
  Q_PROPERTY(int selectedRow READ selected_row WRITE setSelectedRow NOTIFY
                 selectedRowChanged)

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

  double duration() const;
  double playhead() const { return playhead_; }
  double pixels_per_second() const { return pixels_per_second_; }
  int track_count() const { return track_count_; }
  int selected_row() const { return selected_row_; }

  void setPlayhead(double seconds);
  void setPixelsPerSecond(double pps);
  void setSelectedRow(int row);

  // Appends a clip; the timeline's duration grows to cover it.
  Q_INVOKABLE void addClip(const QString& name, int track, double start,
                           double duration, const QString& kind);
  // Moves a clip along its track. Returns the clamped start it landed on.
  Q_INVOKABLE double moveClip(int row, double new_start);
  Q_INVOKABLE void removeClip(int row);
  Q_INVOKABLE void clear();
  // Fills the panel with a few clips so the skeleton has something to show
  // before a project-file loader exists. Callers replace this wholesale.
  Q_INVOKABLE void loadDemo();
  // Row of the clip covering |seconds| on |track|, or -1.
  Q_INVOKABLE int clipAt(double seconds, int track) const;
  // Row of the topmost clip covering |seconds|, or -1 (video tracks first).
  Q_INVOKABLE int topClipAt(double seconds) const;

 signals:
  void durationChanged();
  void playheadChanged();
  void pixelsPerSecondChanged();
  void selectedRowChanged();

 private:
  void RecomputeDuration();

  QList<Clip> clips_;
  double playhead_{0.0};
  double pixels_per_second_{60.0};
  int track_count_{4};
  int selected_row_{-1};
};

}  // namespace avbase::apps

#endif  // AVBASE_APPS_PLAYER_QT_TIMELINE_MODEL_H_
