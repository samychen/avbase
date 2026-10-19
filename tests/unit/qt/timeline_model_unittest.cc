// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The model behind the Qt player's timeline panel. It is the only piece of that
// skeleton with arithmetic in it, and -- being a QAbstractListModel rather than
// a widget -- the only piece that can be checked without a screen.
//
// These tests exist because the panel used to be filled with six hard-coded
// clip names, so it described a project that did not exist. "The clips must
// come from the open file" is not a property a screenshot can check: a
// wrong-but- plausible timeline looks exactly like a right one.

#include "apps/player_qt/timeline_model.h"

#include <QModelIndex>
#include <QString>
#include <QVariant>

#include <gtest/gtest.h>

namespace avbase::apps {
namespace {

class TimelineModelTest : public ::testing::Test {
 protected:
  QVariant Field(int row, int role) const {
    return timeline_.data(timeline_.index(row, 0), role);
  }
  QString name(int row) const {
    return Field(row, TimelineModel::kNameRole).toString();
  }
  int track(int row) const {
    return Field(row, TimelineModel::kTrackRole).toInt();
  }
  double start(int row) const {
    return Field(row, TimelineModel::kStartRole).toDouble();
  }
  double clip_length(int row) const {
    return Field(row, TimelineModel::kDurationRole).toDouble();
  }

  TimelineModel timeline_;
};

// Nothing is open, so there is nothing to draw -- and the panel says so by
// being empty rather than by inventing a project.
TEST_F(TimelineModelTest, IsEmptyBeforeAnyMediaIsOpened) {
  EXPECT_EQ(timeline_.rowCount(), 0);
  // A floor rather than zero: a ruler with no width reads as a broken panel.
  EXPECT_GE(timeline_.duration(), 10.0);
}

// The whole point: the lanes come from the file.
TEST_F(TimelineModelTest, LoadingAMediaBuildsOneLanePerKind) {
  timeline_.loadFromMedia(QStringLiteral("test5.mp4"), 30.03,
                          /*has_video=*/true,
                          /*has_audio=*/true);

  ASSERT_EQ(timeline_.rowCount(), 2);
  EXPECT_EQ(name(0), QStringLiteral("test5.mp4"));
  EXPECT_EQ(name(1), QStringLiteral("test5.mp4"));
  // V1 then A1: the picture sits directly above the sound that goes with it.
  EXPECT_EQ(track(0), 1);
  EXPECT_EQ(track(1), 2);
  EXPECT_EQ(start(0), 0.0);
  EXPECT_EQ(start(1), 0.0);
  EXPECT_DOUBLE_EQ(clip_length(0), 30.03);
  EXPECT_DOUBLE_EQ(clip_length(1), 30.03);
}

// The project's length is the MEDIA's length. Deriving it from the clips is
// what made the playhead stop moving at 18 s on a 30 s file: the clips were
// placeholder data, and the playhead's clamp inherited their length.
TEST_F(TimelineModelTest, TheProjectDurationIsTheMediasOwn) {
  timeline_.loadFromMedia(QStringLiteral("test5.mp4"), 30.03,
                          /*has_video=*/true,
                          /*has_audio=*/true);

  EXPECT_DOUBLE_EQ(timeline_.duration(), 30.03);

  timeline_.setPlayhead(45.0);
  EXPECT_DOUBLE_EQ(timeline_.playhead(), 30.03);

  timeline_.setPlayhead(-5.0);
  EXPECT_DOUBLE_EQ(timeline_.playhead(), 0.0);
}

// A silent film gets a picture lane and no sound lane -- not an empty panel
// and not a placeholder one.
TEST_F(TimelineModelTest, AVideoOnlySourceHasNoAudioLane) {
  timeline_.loadFromMedia(QStringLiteral("silent.mp4"), 12.5,
                          /*has_video=*/true, /*has_audio=*/false);

  ASSERT_EQ(timeline_.rowCount(), 1);
  EXPECT_EQ(track(0), 1);
  EXPECT_DOUBLE_EQ(clip_length(0), 12.5);
}

// A live stream has no length, so it has no ruler to draw clips on. Clearing
// is the honest answer; a made-up duration would not be.
TEST_F(TimelineModelTest, ASourceWithoutADurationClearsThePanel) {
  timeline_.loadFromMedia(QStringLiteral("test5.mp4"), 30.03,
                          /*has_video=*/true,
                          /*has_audio=*/true);
  ASSERT_EQ(timeline_.rowCount(), 2);

  timeline_.loadFromMedia(QStringLiteral("live.m3u8"), 0.0, /*has_video=*/true,
                          /*has_audio=*/true);

  EXPECT_EQ(timeline_.rowCount(), 0);
  EXPECT_GE(timeline_.duration(), 10.0);
}

// Opening a second file must not leave the first one's lanes behind, which is
// how a panel "shows the media" while showing the wrong media.
TEST_F(TimelineModelTest, OpeningAnotherMediaReplacesTheLanes) {
  timeline_.loadFromMedia(QStringLiteral("first.mp4"), 30.03,
                          /*has_video=*/true, /*has_audio=*/true);
  timeline_.loadFromMedia(QStringLiteral("second.mp4"), 8.0,
                          /*has_video=*/true, /*has_audio=*/false);

  ASSERT_EQ(timeline_.rowCount(), 1);
  EXPECT_EQ(name(0), QStringLiteral("second.mp4"));
  EXPECT_DOUBLE_EQ(timeline_.duration(), 8.0);
}

// The playhead is the player's clock, so it is clamped to what is PLAYABLE.
TEST_F(TimelineModelTest, ThePlayheadIsClampedToTheMediaLength) {
  timeline_.loadFromMedia(QStringLiteral("short.mp4"), 4.0, /*has_video=*/true,
                          /*has_audio=*/true);

  timeline_.setPlayhead(999.0);
  EXPECT_DOUBLE_EQ(timeline_.playhead(), 4.0);
}

// Zoom is a view setting; it must not be able to rewrite the model.
TEST_F(TimelineModelTest, ZoomIsClampedAndDoesNotTouchTheClips) {
  timeline_.loadFromMedia(QStringLiteral("test5.mp4"), 30.03,
                          /*has_video=*/true, /*has_audio=*/true);

  timeline_.setPixelsPerSecond(99999.0);
  EXPECT_LE(timeline_.pixels_per_second(), 600.0);
  EXPECT_DOUBLE_EQ(clip_length(0), 30.03);
  EXPECT_EQ(timeline_.rowCount(), 2);
}

}  // namespace
}  // namespace avbase::apps
