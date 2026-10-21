// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/qt/timeline_model.h"

#include <algorithm>

// Qt Core only. The colours are hex strings handed to QML, so this model needs
// no QColor -- and it must not pull in QtGui, because that is what lets the
// suite below run headless on a machine with no display.
namespace avbase::qt {

namespace {

// The four lanes the QML header column draws, top to bottom: V2, V1, A1, A2.
// The programme's picture goes on V1 -- the lane immediately above the first
// audio lane -- so what the user is watching sits next to the sound that
// belongs to it, which is where every NLE puts it.
constexpr int kVideoLane = 1;
constexpr int kAudioLane = 2;

// A floor, so a panel with nothing in it is still a ruler: a zero-width
// timeline is indistinguishable from a broken one, and it leaves nowhere to
// drop the first clip.
constexpr double kMinProjectDuration = 10.0;

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
  // The media's own length, or a floor for the empty panel: a ruler with no
  // width reads as a broken one, and leaves nowhere to drop a first clip.
  //
  // Not derived from the clips. It used to be, and that is what let the
  // playhead stop moving at 18 s on a 30 s file -- the clips were placeholder
  // data and the playhead's clamp inherited their length. The panel's length
  // is a property of the media, so it is kept as one.
  return project_duration_ > 0.0 ? project_duration_ : kMinProjectDuration;
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

void TimelineModel::loadFromMedia(const QString& name, double duration,
                                  bool has_video, bool has_audio) {
  beginResetModel();
  clips_.clear();
  // A zero or negative duration means the source never said how long it is;
  // there is then no ruler to put a clip on, so the panel stays empty rather
  // than inventing a length for it.
  project_duration_ = duration > 0.0 ? duration : 0.0;
  if (project_duration_ > 0.0) {
    // One clip per KIND, not one per stream: avbase renders a single video
    // track and a single audio track at a time. Drawing every audio stream as
    // its own lane would claim the app can mix them, which it cannot.
    if (has_video) {
      AppendClip(name, kVideoLane, 0.0, project_duration_,
                 QStringLiteral("video"));
    }
    if (has_audio) {
      AppendClip(name, kAudioLane, 0.0, project_duration_,
                 QStringLiteral("audio"));
    }
  }
  endResetModel();
  setPlayhead(0.0);
  emit durationChanged();
}

void TimelineModel::clear() {
  beginResetModel();
  clips_.clear();
  project_duration_ = 0.0;
  endResetModel();
  setPlayhead(0.0);
  emit durationChanged();
}

void TimelineModel::AppendClip(const QString& name, int track, double start,
                               double duration, const QString& kind) {
  Clip clip;
  clip.name = name;
  clip.track = std::clamp(track, 0, track_count_ - 1);
  clip.start = std::max(0.0, start);
  clip.duration = std::max(0.0, duration);
  clip.kind = kind;
  clip.color = DefaultColor(kind, clip.track);
  clips_.append(clip);
}

}  // namespace avbase::qt
