// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/qt/gl_video_item.h"

#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QQuickOpenGLUtils>
#include <QTimer>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "base/logging.h"
#include "media/base/video_frame.h"
#include "player/qt/gl_video_renderer.h"

namespace avbase::qt {

namespace {

using Clock = std::chrono::steady_clock;

bool TimingEnabled() {
  // Read once: this sits in the per-frame path, and the point of the number is
  // to measure our work rather than getenv's.
  static const bool enabled = qEnvironmentVariableIsSet("AVBASE_QT_TIMING");
  return enabled;
}

// The renderer, and the only part of the GPU path that knows Qt exists.
//
// It owns the frame hand-off (the mailbox), the framebuffer Qt asks for, and
// the fallback when GL does not come up. Everything that actually draws lives
// in GlVideoRenderer, which cannot include a Qt GL header -- see the header of
// gl_video_renderer.h for the collision that forces that split.
class GlRenderer final : public QQuickFramebufferObject::Renderer {
 public:
  GlRenderer() = default;

  ~GlRenderer() override {
    // The GL objects go here, with the context current, which is the contract
    // QQuickFramebufferObject gives a Renderer: it is deleted while its context
    // is still usable, not after the window is gone.
    video_.reset();
    if (TimingEnabled() && frames_ > 0) {
      std::fprintf(stderr,
                   "[qt-gl] frames=%d upload_avg=%.3fms draw_avg=%.3fms\n",
                   frames_, upload_ms_ / frames_, draw_ms_ / frames_);
    }
  }

  GlRenderer(const GlRenderer&) = delete;
  GlRenderer& operator=(const GlRenderer&) = delete;

  QOpenGLFramebufferObject*
  createFramebufferObject(const QSize& size) override {
    QOpenGLFramebufferObjectFormat format;
    // One flat quad, no depth testing, no stencil, no multisampling: samples
    // would make the driver allocate and resolve a second buffer every frame
    // to antialias edges this item does not have.
    format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    format.setSamples(0);
    return new QOpenGLFramebufferObject(size, format);
  }

  // Runs on the render thread with the GUI thread blocked, so reading the
  // item's C++ state is safe. Taking the frame here rather than in render()
  // means the hand-off happens once per sync instead of once per repaint of an
  // unchanged picture -- and a repaint happens every time the control bar
  // fades, which is not a moment to re-upload 3 MB of planes.
  void synchronize(QQuickFramebufferObject* item) override {
    auto* self = static_cast<GlVideoItem*>(item);
    item_ = self;
    FrameMailbox* mailbox = self->mailbox();
    if (mailbox == nullptr) {
      return;
    }
    int64_t serial = 0;
    base::scoped_refptr<media::VideoFrame> frame = mailbox->Load(&serial);
    if (serial == mailbox_serial_) {
      return;
    }
    mailbox_serial_ = serial;
    frame_ = std::move(frame);
    pending_upload_ = true;
    rotation_ = frame_ ? frame_->rotation() : 0;
  }

  void render() override {
    QOpenGLFramebufferObject* fbo = framebufferObject();
    const QSize size = fbo != nullptr ? fbo->size() : QSize();
    if (size.isEmpty()) {
      return;
    }
    if (!EnsureInitialized(size)) {
      PaintFallback(size);
      return;
    }

    if (pending_upload_) {
      pending_upload_ = false;
      if (frame_) {
        const auto begin = Clock::now();
        if (!video_->Upload(*frame_)) {
          // Say it rather than paint the previous picture forever, which looks
          // like a hang. The item still shows the last good frame; the log is
          // the only place the format shows up.
          LOG(ERROR) << "qt-gl: upload refused for "
                     << media::GetVideoFormatName(frame_->format());
          frame_.reset();
        }
        if (TimingEnabled()) {
          upload_ms_ +=
              std::chrono::duration<double, std::milli>(Clock::now() - begin)
                  .count();
        }
      }
    }

    if (video_->has_picture()) {
      const auto begin = Clock::now();
      video_->Paint(size.width(), size.height(), rotation_);
      if (TimingEnabled()) {
        draw_ms_ +=
            std::chrono::duration<double, std::milli>(Clock::now() - begin)
                .count();
      }
    } else {
      PaintFallback(size);
    }
    // QSG caches the GL state it believes it is in, and raw GL calls made
    // behind its back leave that cache wrong. This is the one function Qt
    // documents for handing the state back; skipping it shows up later as an
    // unrelated item blending or depth-testing wrongly.
    QQuickOpenGLUtils::resetOpenGLState();
    ++frames_;
  }

 private:
  bool EnsureInitialized(const QSize& size) {
    if (init_attempted_) {
      return video_ != nullptr;
    }
    init_attempted_ = true;
    QOpenGLContext* current = QOpenGLContext::currentContext();
    if (current == nullptr) {
      LOG(ERROR) << "qt-gl: no current OpenGL context on the render thread";
      Report(QStringLiteral("no GL context"));
      return false;
    }
    auto video = std::make_unique<GlVideoRenderer>();
    std::string error;
    if (!video->Init(current, size.width(), size.height(), &error)) {
      LOG(ERROR) << "qt-gl: renderer init failed: " << error;
      Report(QStringLiteral("gl unavailable"));
      return false;
    }
    video_ = std::move(video);
    Report(QString::fromStdString(video_->renderer_name()));
    return true;
  }

  // Nothing of ours is drawable: still leave a defined background rather than
  // whatever the driver happened to have in the buffer. QOpenGLFunctions is
  // the ES2 subset and needs no entry-point loading, which is why it is the
  // right tool for the branch where glad did not load.
  void PaintFallback(const QSize& size) {
    QOpenGLContext* current = QOpenGLContext::currentContext();
    if (current == nullptr) {
      return;
    }
    QOpenGLFunctions* f = current->functions();
    f->glViewport(0, 0, size.width(), size.height());
    f->glClearColor(12.0f / 255.0f, 12.0f / 255.0f, 14.0f / 255.0f, 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);
    QQuickOpenGLUtils::resetOpenGLState();
  }

  void Report(const QString& info) {
    // Cross-thread by construction: this runs on the render thread and the
    // setter lives on the GUI thread. A queued invocation is safe even if the
    // item dies before delivery -- Qt drops posted events whose receiver is
    // gone -- so the captured pointer needs no guard.
    if (item_ == nullptr) {
      return;
    }
    QMetaObject::invokeMethod(
        item_, [item = item_, info]() { item->reportRendererState(info); },
        Qt::QueuedConnection);
  }

  std::unique_ptr<GlVideoRenderer> video_;
  // Set in synchronize(), the one point Qt hands the item over. Used only to
  // post the init outcome back to the GUI thread.
  GlVideoItem* item_{nullptr};

  bool init_attempted_{false};
  bool pending_upload_{false};
  int64_t mailbox_serial_{-1};
  int rotation_{0};
  base::scoped_refptr<media::VideoFrame> frame_;

  int frames_{0};
  double upload_ms_{0.0};
  double draw_ms_{0.0};
};

}  // namespace

GlVideoItem::GlVideoItem(QQuickItem* parent) : QQuickFramebufferObject(parent) {
  // The picture would otherwise be upside down, and nothing would say so: GL
  // draws with the origin at the BOTTOM-left, Qt composites this item's
  // framebuffer assuming rows start at the top. Setting this flag makes Qt do
  // the flip when it samples the texture, which is one line here instead of a
  // coordinate twist inside the shader -- but either way it is a fact about
  // orientation, so it is pinned by a pixel comparison (AVBASE_QT_GRAB_REF)
  // and not by this comment: the mistake produces a perfectly valid, perfectly
  // inverted image with no GL error and no log line.
  setMirrorVertically(true);

  // The same 16 ms poll as the software item, and for the same reason: the
  // mailbox cannot wake anyone, so somebody has to look. What differs is the
  // cost of a look -- a lock and a serial compare, never a pixel.
  timer_ = new QTimer(this);
  timer_->setInterval(16);
  timer_->setTimerType(Qt::PreciseTimer);
  QObject::connect(timer_, &QTimer::timeout, this, &GlVideoItem::refresh);
  timer_->start();
}

GlVideoItem::~GlVideoItem() = default;

void GlVideoItem::setMailbox(FrameMailbox* mailbox) {
  mailbox_.store(mailbox, std::memory_order_release);
  last_seen_serial_ = -1;
}

QQuickFramebufferObject::Renderer* GlVideoItem::createRenderer() const {
  return new GlRenderer();
}

void GlVideoItem::refresh() {
  FrameMailbox* box = mailbox();
  if (box == nullptr) {
    return;
  }
  const int64_t serial = box->Serial();
  if (serial == last_seen_serial_) {
    return;
  }
  last_seen_serial_ = serial;

  // Peeked here only for the readout QML shows; not one pixel is touched, and
  // the upload happens on the render thread from its own load.
  base::scoped_refptr<media::VideoFrame> frame = box->Load(nullptr);
  const bool had_frame = has_frame_;
  bool changed = false;
  if (!frame) {
    has_frame_ = false;
    if (had_frame) {
      frame_info_.clear();
      changed = true;
    }
  } else {
    has_frame_ = true;
    const media::Size coded = frame->coded_size();
    const QString info =
        QStringLiteral("%1x%2 %3")
            .arg(coded.width)
            .arg(coded.height)
            .arg(QString::fromUtf8(media::GetVideoFormatName(frame->format())));
    if (info != frame_info_) {
      frame_info_ = info;
      changed = true;
    }
  }
  update();
  if (changed) {
    emit frameChanged();
  }
}

void GlVideoItem::reportRendererState(const QString& info) {
  if (renderer_info_ == info) {
    return;
  }
  renderer_info_ = info;
  emit rendererInfoChanged();
}

}  // namespace avbase::qt
