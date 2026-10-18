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
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QSurfaceFormat>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "apps/player_qt/frame_mailbox.h"
#include "apps/player_qt/gl_video_item.h"
#include "apps/player_qt/player_bridge.h"
#include "apps/player_qt/timeline_model.h"
#include "apps/player_qt/video_item.h"

namespace {

// Set by CMake to the source qml/ directory. Loading from the filesystem rather
// than from a compiled-in resource keeps the QML editable while the skeleton is
// being worked on: change a file, restart, see it. A shipped product would flip
// this to a qrc path and gain the single-binary property.
constexpr char kQmlDir[] = AVBASE_QT_QML_DIR;

// Mean absolute per-channel difference between two grabs, as the oracle for
// "the GPU path draws the same picture as the software path". A pixel-exact
// match is not the goal and would be a red flag: two different YUV->RGB
// implementations that agree bit for bit are more likely to be one
// implementation called twice. What this catches is the failures that matter
// and that a screenshot review misses -- upside down, wrong aspect, wrong
// colour matrix, half the picture missing.
double MeanAbsDifference(const QImage& a, const QImage& b, int* max_delta) {
  if (max_delta != nullptr) {
    *max_delta = 0;
  }
  if (a.size() != b.size() || a.isNull()) {
    return -1.0;
  }
  const QImage lhs = a.convertToFormat(QImage::Format_RGB32);
  const QImage rhs = b.convertToFormat(QImage::Format_RGB32);
  double sum = 0.0;
  int peak = 0;
  qint64 samples = 0;
  for (int y = 0; y < lhs.height(); ++y) {
    const auto* l = reinterpret_cast<const QRgb*>(lhs.constScanLine(y));
    const auto* r = reinterpret_cast<const QRgb*>(rhs.constScanLine(y));
    for (int x = 0; x < lhs.width(); ++x) {
      const int dr = std::abs(qRed(l[0]) - qRed(r[0]));
      const int dg = std::abs(qGreen(l[0]) - qGreen(r[0]));
      const int db = std::abs(qBlue(l[0]) - qBlue(r[0]));
      sum += dr + dg + db;
      samples += 3;
      peak = std::max({peak, dr, dg, db});
      ++l;
      ++r;
    }
  }
  if (max_delta != nullptr) {
    *max_delta = peak;
  }
  return samples == 0 ? -1.0 : sum / static_cast<double>(samples);
}

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
  qmlRegisterType<avbase::apps::GlVideoItem>("AvbasePlayer", 1, 0,
                                             "GlVideoItem");

  // Pin the scene graph to OpenGL rather than letting Qt choose.
  //
  // Qt 6 defaults to Metal on macOS and D3D11 on Windows, and
  // QQuickFramebufferObject's renderer is handed a raw GL context: on any other
  // backend there is no GL context to hand over and the video item has nothing
  // to draw into. So the choice is not "GL or the platform default" for this
  // app -- it is "GL, or no GPU path at all". Stating it here, before any
  // window exists, is the only place it can be stated.
  //
  // The cost is honest and bounded: everything else in the window (the control
  // bar, the timeline, text) then also renders through Qt's OpenGL backend,
  // which Qt supports fully. A product that wanted Metal for its chrome would
  // have to reach the RHI instead of GL -- and would then not be using
  // render/gl, which is the module this skeleton exists to exercise.
  QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

  // Ask for a core profile BEFORE any window exists.
  //
  // Measured, not guessed: with Qt's defaults on macOS the scene graph gets a
  // 2.1 compatibility context ("context reports 2.1 INTEL-23.7.4"), and
  // avbase_gl refuses anything below OpenGL 3.3 core -- so the GPU item reports
  // "gl unavailable" and paints a flat background while every other Qt test
  // stays green. macOS offers exactly two choices here, 2.1 legacy and 3.2+
  // core, so asking for 3.3 core is what selects the second one; the driver
  // then hands back 4.1 core, which is what this machine reports.
  //
  // QSurfaceFormat::setDefaultFormat() is the only place this can be said:
  // once a QQuickWindow exists its context is already created with whatever
  // format was in force at that moment.
  QSurfaceFormat format;
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  // Depth and stencil are Qt Quick's business, not ours (our framebuffer asks
  // for neither), but the default format is now global, so it has to cover
  // what the rest of the scene graph may ask for. 24/8 is Qt's own default.
  format.setDepthBufferSize(24);
  format.setStencilBufferSize(8);
  QSurfaceFormat::setDefaultFormat(format);

  // Which surface to build. Opt-in rather than default, because the software
  // path is the oracle the GPU path is checked against and needs to stay one
  // environment variable away.
  const bool use_gl_surface = qEnvironmentVariableIsSet("AVBASE_QT_GL");
  std::fprintf(stderr, "video surface: %s\n",
               use_gl_surface ? "opengl (QQuickFramebufferObject)"
                              : "software (QPainter)");

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
  engine.rootContext()->setContextProperty(QStringLiteral("useGlSurface"),
                                           use_gl_surface);

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
  //
  // Two types, two lookups: Main.qml builds exactly one of them, so at most one
  // of these succeeds. Both names are checked rather than branching on the flag
  // again, which keeps "which surface did QML actually build" a fact this code
  // observes instead of a fact it assumes.
  QObject* const root = engine.rootObjects().first();
  bool handed_over = false;
  if (auto* soft = root->findChild<avbase::apps::VideoItem*>(
          QStringLiteral("videoSurface"))) {
    soft->setMailbox(&mailbox);
    handed_over = true;
  }
  if (auto* gpu = root->findChild<avbase::apps::GlVideoItem*>(
          QStringLiteral("glVideoSurface"))) {
    gpu->setMailbox(&mailbox);
    handed_over = true;
  }
  if (!handed_over) {
    std::fprintf(stderr,
                 "Main.qml built no video surface (neither videoSurface nor "
                 "glVideoSurface)\n");
    return 1;
  }

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
  //
  // AVBASE_QT_GRAB_REF=<earlier grab> turns the grab into a comparison: the two
  // images are the same scene rendered through two different video paths, so
  // the difference measures the paths and nothing else. See
  // MeanAbsDifference() above for what the number is good for.
  //
  // AVBASE_QT_GRAB_SEEK=<seconds> pauses and seeks before grabbing, which is
  // what makes the comparison an oracle at all: a moving picture grabbed twice
  // at "2.5 seconds after start" differs by however many frames the two runs
  // drifted apart, and that noise is an order of magnitude larger than any
  // difference between the renderers. Seeking first pins both runs to the same
  // decoded frame, so what is left is the paths.
  const QByteArray grab_path = qgetenv("AVBASE_QT_GRAB");
  if (!grab_path.isEmpty()) {
    const int delay_ms = qEnvironmentVariableIntValue("AVBASE_QT_GRAB_DELAY");
    QTimer::singleShot(
        delay_ms > 0 ? delay_ms : 1500, &app,
        [&engine, &app, &bridge, grab_path]() {
          auto do_grab = [&engine, &app, grab_path]() {
            QObject* root = engine.rootObjects().value(0);
            if (auto* window = qobject_cast<QQuickWindow*>(root)) {
              const QImage image = window->grabWindow();
              const QString path = QString::fromLocal8Bit(grab_path);
              const bool saved = image.save(path);
              std::fprintf(stderr, "grabbed %dx%d to %s: %s\n", image.width(),
                           image.height(), qPrintable(path),
                           saved ? "ok" : "failed");
              const QByteArray ref_path = qgetenv("AVBASE_QT_GRAB_REF");
              if (!ref_path.isEmpty()) {
                const QImage reference(QString::fromLocal8Bit(ref_path));
                int peak = 0;
                const double mean = MeanAbsDifference(image, reference, &peak);
                if (mean < 0.0) {
                  std::fprintf(stderr,
                               "compare: sizes differ or reference unreadable "
                               "(%dx%d vs %dx%d)\n",
                               image.width(), image.height(), reference.width(),
                               reference.height());
                } else {
                  std::fprintf(stderr, "compare: mean_abs=%.3f max_abs=%d\n",
                               mean, peak);
                }
              }
            } else {
              std::fprintf(stderr, "root object is not a window\n");
            }
            app.quit();
          };

          const QByteArray seek_s = qgetenv("AVBASE_QT_GRAB_SEEK");
          if (seek_s.isEmpty()) {
            do_grab();
            return;
          }
          const double seconds = QString::fromLocal8Bit(seek_s).toDouble();
          if (bridge.playing()) {
            bridge.playPause();
          }
          bridge.seekCommit(seconds);
          std::fprintf(stderr, "seeking to %.3fs before grab\n", seconds);
          // Long enough for the seek to round-trip through the pipeline and
          // the sink to present the frame there. Fixed rather than polled so
          // both compared runs wait exactly as long.
          //
          // Overridable because two questions need different answers: "is the
          // picture the seek target?" wants the shortest wait that still
          // settles, while "does the picture STAY there while paused?" wants
          // a wait long enough for a resumed pipeline to have run away
          // visibly.
          const int seek_wait_ms =
              qEnvironmentVariableIntValue("AVBASE_QT_GRAB_SEEK_WAIT");
          QTimer::singleShot(seek_wait_ms > 0 ? seek_wait_ms : 800, &app,
                             do_grab);
        });
  }

  return app.exec();
}
