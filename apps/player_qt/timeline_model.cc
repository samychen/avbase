// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/player_qt/timeline_model.h"

#include <algorithm>

#include <QColor>

namespace avbase::apps {

namespace {

// Track colours follow the convention every NLE uses, because a timeline the
// user has to re-learn is a worse timeline: video above audio, and the video
// stack darkens towards the background.
QString DefaultColor(const QString& kind, int track) {
  if (kind == QStringLiteral("audio")) {
    return QStringLiteral("#2e7d5b");
  }
  if (kind == QStringLiteral("image")) {
    return QStringLiteral("#7a5aa8");
  }
  static const char* kVideo[] = {"#3f6fb5", "#4a7fc4", "#568fd3"};
  return QString::fromLatin1(kVideo[std::clamp(track, 0, 2)]);
}

}  // namespace

TimelineModel::TimelineModel(QObject* parent) : QAbstractListModel(parent) {}

int TimelineModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(clips_.size());
}

QVariant TimelineModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 ||
      index.row() >= static_cast<int>(clips_.size())) {
    return QVariant();
  }
  const Clip& clip = clips_[index.row()];
  switch (role) {
  case kNameRole:
    return clip.name;
  case kTrackRole:
    return clip.track;
  case kStartRole:
    return clip.start;
  case kDurationRole:
    return clip.duration;
  case kKindRole:
    return clip.kind;
  case kColorRole:
    return clip.color;
  default:
    return QVariant();
  }
}

QHash<int, QByteArray> TimelineModel::roleNames() const {
  return {
      {kNameRole, "name"},   {kTrackRole, "track"},
      {kStartRole, "start"}, {kDurationRole, "clipLength"},
      {kKindRole, "kind"},   {kColorRole, "color"},
  };
}

double TimelineModel::duration() const {
  double total = 0.0;
  for (const Clip& clip : clips_) {
    total = std::max(total, clip.start + clip.duration);
  }
  // A floor keeps an empty timeline from collapsing the ruler to zero width,
  // which makes the first clip impossible to drag to a sensible place.
  return std::max(total, 10.0);
}

void TimelineModel::setPlayhead(double seconds) {
  const double clamped = std::clamp(seconds, 0.0, duration());
  if (qFuzzyCompare(clamped, playhead_)) {
    return;
  }
  playhead_ = clamped;
  emit playheadChanged();
}

void TimelineModel::setPixelsPerSecond(double pps) {
  const double clamped = std::clamp(pps, 4.0, 600.0);
  if (qFuzzyCompare(clamped, pixels_per_second_)) {
    return;
  }
  pixels_per_second_ = clamped;
  emit pixelsPerSecondChanged();
}

void TimelineModel::setSelectedRow(int row) {
  if (row == selected_row_) {
    return;
  }
  selected_row_ = row;
  emit selectedRowChanged();
}

void TimelineModel::addClip(const QString& name, int track, double start,
                            double duration, const QString& kind) {
  Clip clip;
  clip.name = name;
  clip.track = std::clamp(track, 0, track_count_ - 1);
  clip.start = std::max(0.0, start);
  clip.duration = std::max(0.1, duration);
  clip.kind = kind;
  clip.color = DefaultColor(kind, clip.track);

  const int row = static_cast<int>(clips_.size());
  beginInsertRows(QModelIndex(), row, row);
  clips_.append(clip);
  endInsertRows();
  RecomputeDuration();
}

double TimelineModel::moveClip(int row, double new_start) {
  if (row < 0 || row >= static_cast<int>(clips_.size())) {
    return 0.0;
  }
  const double clamped = std::max(0.0, new_start);
  if (qFuzzyCompare(clamped, clips_[row].start)) {
    return clamped;
  }
  clips_[row].start = clamped;
  const QModelIndex changed = index(row, 0);
  emit dataChanged(changed, changed, {kStartRole});
  RecomputeDuration();
  return clamped;
}

void TimelineModel::removeClip(int row) {
  if (row < 0 || row >= static_cast<int>(clips_.size())) {
    return;
  }
  beginRemoveRows(QModelIndex(), row, row);
  clips_.removeAt(row);
  endRemoveRows();
  if (selected_row_ == row) {
    setSelectedRow(-1);
  } else if (selected_row_ > row) {
    setSelectedRow(selected_row_ - 1);
  }
  RecomputeDuration();
}

void TimelineModel::clear() {
  if (clips_.isEmpty()) {
    return;
  }
  beginResetModel();
  clips_.clear();
  endResetModel();
  setSelectedRow(-1);
  setPlayhead(0.0);
  RecomputeDuration();
}

void TimelineModel::loadDemo() {
  beginResetModel();
  clips_.clear();
  endResetModel();

  // Two video tracks and one audio track, which is the shape a first cut has:
  // a programme track, an overlay track, and the sound.
  addClip(QStringLiteral("intro.mp4"), 0, 0.0, 6.0, QStringLiteral("video"));
  addClip(QStringLiteral("b-roll.mp4"), 1, 2.5, 5.0, QStringLiteral("video"));
  addClip(QStringLiteral("interview.mp4"), 0, 6.0, 12.0,
          QStringLiteral("video"));
  addClip(QStringLiteral("logo.png"), 1, 9.0, 4.0, QStringLiteral("image"));
  addClip(QStringLiteral("music.wav"), 2, 0.0, 18.0, QStringLiteral("audio"));
  addClip(QStringLiteral("voice.wav"), 3, 6.0, 12.0, QStringLiteral("audio"));
}

int TimelineModel::clipAt(double seconds, int track) const {
  for (int row = 0; row < static_cast<int>(clips_.size()); ++row) {
    const Clip& clip = clips_[row];
    if (clip.track == track && seconds >= clip.start &&
        seconds < clip.start + clip.duration) {
      return row;
    }
  }
  return -1;
}

int TimelineModel::topClipAt(double seconds) const {
  for (int track = 0; track < track_count_; ++track) {
    const int row = clipAt(seconds, track);
    if (row >= 0) {
      return row;
    }
  }
  return -1;
}

void TimelineModel::RecomputeDuration() {
  emit durationChanged();
}

}  // namespace avbase::apps
