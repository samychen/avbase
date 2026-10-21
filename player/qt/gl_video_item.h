// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The QML video surface, GPU version: a QQuickFramebufferObject whose renderer
// uploads the newest frame's three planes as textures and draws them through
// avbase's own YUV->RGB shader, inside Qt's framebuffer, on Qt's render thread.
//
// WHY THIS REPLACES video_item.cc RATHER THAN EXTENDING IT. The software item
// converts every pixel on the CPU and then hands QPainter a QImage: at 1080p
// that is two million pixels of fixed-point maths per frame, and at 4K eight
// million, all of it on the GUI thread. Here the same conversion is one
// fragment-shader pass on the GPU and the only per-frame CPU work is the
// three glTexSubImage2D calls the decoder's bytes force anyway. The two items
// stay side by side because the software one is the ORACLE: it is simple
// enough to be obviously right, and the GPU one is checked against its output
// with AVBASE_QT_GRAB_REF (see main.cc).
//
// WHAT DOES NOT TRAVEL THROUGH GlPresenter. GlPresenter ends in "blit the
// canvas to the default framebuffer and swap", and inside a Quick item there
// is neither: Qt bound an FBO for us and Qt swaps when the whole window is
// composed. So this renderer drives the parts itself -- GlApi, the layer, the
// program, the quad -- and stops before the present. That is the same split
// GlPresenter makes internally; the difference is only who calls it.
//
// THREADING. synchronize() and render() run on Qt's scene-graph render thread,
// which is where the GL context is current -- and that is exactly the thread
// GlVideoLayer documents. The item's own properties are read on the GUI
// thread; the only thing crossing between them is the FrameMailbox, which is
// already a mutex-guarded one-slot hand-off.

#ifndef AVBASE_PLAYER_QT_GL_VIDEO_ITEM_H_
#define AVBASE_PLAYER_QT_GL_VIDEO_ITEM_H_

#include <stdint.h>

#include <atomic>

#include <QQuickFramebufferObject>
#include <QString>

#include "player/qt/frame_mailbox.h"

class QTimer;

namespace avbase::gl {
class GlApi;
class GlProgram;
class GlQuad;
}  // namespace avbase::gl

namespace avbase::media {
class GlVideoLayer;
class QtGLContext;
}  // namespace avbase::media

namespace avbase::qt {

class GlVideoItem : public QQuickFramebufferObject {
  Q_OBJECT
  Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)
  Q_PROPERTY(QString frameInfo READ frameInfo NOTIFY frameChanged)
  Q_PROPERTY(QString rendererInfo READ rendererInfo NOTIFY rendererInfoChanged)

 public:
  explicit GlVideoItem(QQuickItem* parent = nullptr);
  ~GlVideoItem() override;

  // |mailbox| must outlive this item. Called once, from main(), after the QML
  // scene exists; the render thread may already be running, which is why the
  // pointer is atomic rather than plain.
  void setMailbox(FrameMailbox* mailbox);

  bool hasFrame() const { return has_frame_; }
  QString frameInfo() const { return frame_info_; }
  // Which GL renderer is actually in use ("gl: <GL_RENDERER>" once init
  // succeeded, "gl unavailable" otherwise). Surfaced because "the OpenGL
  // backend did not come up" and "the file has no video" look identical on
  // screen -- a black rectangle.
  QString rendererInfo() const { return renderer_info_; }

  QQuickFramebufferObject::Renderer* createRenderer() const override;

  // Polls the mailbox for a new serial and schedules a repaint. Called by the
  // internal timer; Q_INVOKABLE so a host can drive it from its own clock.
  Q_INVOKABLE void refresh();

  FrameMailbox* mailbox() const {
    return mailbox_.load(std::memory_order_acquire);
  }

  // Render-thread -> GUI-thread report of the init outcome, posted with a
  // queued invocation because it crosses threads.
  void reportRendererState(const QString& info);

 signals:
  void frameChanged();
  void rendererInfoChanged();

 private:
  std::atomic<FrameMailbox*> mailbox_{nullptr};
  QTimer* timer_{nullptr};
  int64_t last_seen_serial_{-1};
  bool has_frame_{false};
  QString frame_info_;
  QString renderer_info_{QStringLiteral("pending")};
};

}  // namespace avbase::qt

#endif  // AVBASE_PLAYER_QT_GL_VIDEO_ITEM_H_
