// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The QML video surface: a QQuickPaintedItem that drains a FrameMailbox and
// paints the newest frame with QPainter.
//
// QPainter rather than a QSG texture node on purpose. The heavy part of a
// production player is not this: it is either a GPU path (avbase drawing into
// Qt's FBO through a platform/qt/ NativeGLContext) or nothing at all. Writing
// the software path against QQuickItem's raw scene-graph API would add a
// hundred lines of texture bookkeeping that the GPU path throws away. This
// class is the placeholder that proves the wiring, and its public shape
// (setMailbox / refresh / frameSize) is what the GPU version keeps.

#ifndef AVBASE_APPS_PLAYER_QT_VIDEO_ITEM_H_
#define AVBASE_APPS_PLAYER_QT_VIDEO_ITEM_H_

#include <QImage>
#include <QQuickPaintedItem>
#include <QSize>
#include <QString>

#include "apps/player_qt/frame_mailbox.h"

class QTimer;

namespace avbase::apps {

class VideoItem : public QQuickPaintedItem {
  Q_OBJECT
  // Exposed so QML can draw a hint ("no signal") before the first frame.
  Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)
  Q_PROPERTY(QString frameInfo READ frameInfo NOTIFY frameChanged)

 public:
  explicit VideoItem(QQuickItem* parent = nullptr);
  ~VideoItem() override;

  // |mailbox| must outlive this item; the app owns both and destroys the item
  // first (the QML engine is torn down before main()'s locals).
  void setMailbox(FrameMailbox* mailbox);

  bool hasFrame() const { return !image_.isNull(); }
  QString frameInfo() const { return frame_info_; }

  void paint(QPainter* painter) override;

  // Drains the mailbox if a new frame arrived. Called by the internal timer;
  // public so a host can drive it from its own frame clock instead.
  Q_INVOKABLE void refresh();

 signals:
  void frameChanged();

 private:
  FrameMailbox* mailbox_{nullptr};
  QImage image_;
  QString frame_info_;
  QTimer* timer_{nullptr};
  int64_t last_serial_{-1};
};

}  // namespace avbase::apps

#endif  // AVBASE_APPS_PLAYER_QT_VIDEO_ITEM_H_
