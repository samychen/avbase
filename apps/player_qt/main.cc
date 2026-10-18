// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
//   avbase_player_qt [media-file-or-url]
//
// The composition root, and the only file that knows about all four pieces.
//
// DESTRUCTION ORDER IS THE INTERESTING PART. The QML engine must die before the
// PlayerBridge, because the bridge owns the Player and ~Player() joins the
// pipeline threads; and the bridge must die before the FrameMailbox and the
// TimelineModel it points at. Declaring them in this order on the stack gives
// exactly that, in reverse: engine, bridge, timeline, mailbox. A Player given
// static storage duration instead would run that join at atexit and crash --
// see the WATCH OUT in player/public/player.h.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <cstdio>

#include "apps/player_qt/frame_mailbox.h"
#include "apps/player_qt/player_bridge.h"
#include "apps/player_qt/timeline_model.h"
#include "apps/player_qt/video_item.h"

namespace {

// Set by CMake to the source qml/ directory. Loading from the filesystem rather
// than from a compiled-in resource keeps the QML editable while the skeleton is
// being worked on: change a file, restart, see it. A shipped product would flip
// this to a qrc path and gain the single-binary property.
constexpr char kQmlDir[] = AVBASE_QT_QML_DIR;

}  // namespace

int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  QGuiApplication::setApplicationName(QStringLiteral("avbase player"));
  QGuiApplication::setApplicationDisplayName(
      QStringLiteral("avbase player (Qt skeleton)"));

  // Basic, not the platform style: the control bar is a custom overlay and the
  // native styles each impose their own metrics on the buttons inside it, which
  // makes the same QML look different on every OS for no benefit.
  QQuickStyle::setStyle(QStringLiteral("Basic"));

  // The one C++ type QML instantiates directly. PlayerBridge and TimelineModel
  // arrive as context properties instead, because there is exactly one of each
  // and QML should not be able to construct a second Player by accident.
  qmlRegisterType<avbase::apps::VideoItem>("AvbasePlayer", 1, 0, "VideoItem");

  avbase::apps::FrameMailbox mailbox;
  avbase::apps::TimelineModel timeline;
  avbase::apps::PlayerBridge bridge(&timeline);
  // Must precede the first load: the sink factory captures the mailbox at
  // Player construction and cannot be swapped afterwards.
  bridge.setMailbox(&mailbox);
  timeline.loadDemo();

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("player"), &bridge);
  engine.rootContext()->setContextProperty(QStringLiteral("timeline"),
                                           &timeline);

  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
      []() {
        std::fprintf(stderr, "failed to create the QML scene\n");
        QCoreApplication::exit(1);
      },
      Qt::QueuedConnection);

  engine.load(QUrl::fromLocalFile(QStringLiteral("%1/Main.qml").arg(kQmlDir)));
  if (engine.rootObjects().isEmpty()) {
    return 1;
  }

  // The video item is created by QML, so it cannot be handed the mailbox from
  // C++ before it exists. Looking it up by objectName is the standard way to
  // close that loop; the alternative (a context property the item reads) would
  // make the item depend on a global.
  auto* surface =
      engine.rootObjects().first()->findChild<avbase::apps::VideoItem*>(
          QStringLiteral("videoSurface"));
  if (surface == nullptr) {
    std::fprintf(stderr, "Main.qml has no VideoItem named videoSurface\n");
    return 1;
  }
  surface->setMailbox(&mailbox);

  if (argc > 1) {
    bridge.open(QString::fromLocal8Bit(argv[1]));
  }

  // Headless verification hook, inert unless the environment says otherwise.
  // A screen capture proves nothing about a window that may be off-screen or
  // behind another; grabbing the window's own framebuffer is the only check
  // that the QML scene actually painted, and it works under
  // QT_QPA_PLATFORM=offscreen, which is what lets CI use it.
  //
  //   AVBASE_QT_GRAB=/tmp/out.png [AVBASE_QT_GRAB_DELAY=1500] <app> <media>
  const QByteArray grab_path = qgetenv("AVBASE_QT_GRAB");
  if (!grab_path.isEmpty()) {
    const int delay_ms = qEnvironmentVariableIntValue("AVBASE_QT_GRAB_DELAY");
    QTimer::singleShot(
        delay_ms > 0 ? delay_ms : 1500, &app, [&engine, &app, grab_path]() {
          QObject* root = engine.rootObjects().value(0);
          if (auto* window = qobject_cast<QQuickWindow*>(root)) {
            const QImage image = window->grabWindow();
            const QString path = QString::fromLocal8Bit(grab_path);
            const bool saved = image.save(path);
            std::fprintf(stderr, "grabbed %dx%d to %s: %s\n", image.width(),
                         image.height(), qPrintable(path),
                         saved ? "ok" : "failed");
          } else {
            std::fprintf(stderr, "root object is not a window\n");
          }
          app.quit();
        });
  }

  return app.exec();
}
