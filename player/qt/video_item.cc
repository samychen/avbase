// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/qt/video_item.h"

#include <QColor>
#include <QPainter>
#include <QRectF>
#include <QTimer>
#include <QTransform>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "base/logging.h"
#include "media/base/video_color_space.h"
#include "media/base/video_frame.h"

namespace avbase::qt {

namespace {

// Integer BT.709 / BT.601 limited-range coefficients, scaled by 1024. Fixed
// point rather than float: a 1080p frame is two million pixels and this runs
// thirty times a second, so the difference between a multiply-add and a
// double is a measurable slice of a core on a laptop.
struct YuvCoefficients {
  int rv;
  int gu;
  int gv;
  int bu;
};

constexpr YuvCoefficients kBt709{1832, -218, -546,
                                 2163};  // 1.793/0.213/0.533/2.112
constexpr YuvCoefficients kBt601{1634, -400, -832,
                                 2066};  // 1.596/0.391/0.813/2.018

int Clamp8(int value) {
  return value < 0 ? 0 : (value > 255 ? 255 : value);
}

// Which matrix the frame actually means. The container's tag wins; when it is
// unspecified the guess is the same one media/base's GuessColorSpaceFallback
// encodes -- SD-sized material is BT.601, HD and up is BT.709. Getting this
// wrong is not a crash, it is a subtle green/magenta cast, which is why it is
// worth ten lines rather than assuming 709 for everything.
const YuvCoefficients& PickCoefficients(const media::VideoColorSpace& cs,
                                        int height) {
  switch (cs.matrix) {
  case media::ColorMatrix::kSMPTE170M:
    return kBt601;
  case media::ColorMatrix::kBT709:
  case media::ColorMatrix::kBT2020Ncl:
  case media::ColorMatrix::kBT2020Cl:
    return kBt709;
  case media::ColorMatrix::kIdentity:
  case media::ColorMatrix::kUnknown:
    return height >= 720 ? kBt709 : kBt601;
  }
  return kBt709;
}

// I420 -> RGB32 (0xffRRGGBB).
//
// Every plane is read row by row through stride(): visible_data() hands back
// the WHOLE allocation, padding included, so treating it as a tightly packed
// w*h block reads the padding as if it were picture. That mistake is invisible
// at width==stride (1080p is exactly such a case) and shows up as diagonal
// smearing on any width that is not a multiple of the alignment.
QImage I420ToImage(const media::VideoFrame& frame) {
  const media::Size coded = frame.coded_size();
  if (coded.width <= 0 || coded.height <= 0) {
    return QImage();
  }
  const int width = coded.width;
  const int height = coded.height;
  const int chroma_width = (width + 1) / 2;
  const int chroma_height = (height + 1) / 2;

  const std::span<const uint8_t> y_plane =
      frame.visible_data(media::VideoFrame::kYPlane);
  const std::span<const uint8_t> u_plane =
      frame.visible_data(media::VideoFrame::kUPlane);
  const std::span<const uint8_t> v_plane =
      frame.visible_data(media::VideoFrame::kVPlane);

  const int y_stride = frame.stride(media::VideoFrame::kYPlane);
  const int u_stride = frame.stride(media::VideoFrame::kUPlane);
  const int v_stride = frame.stride(media::VideoFrame::kVPlane);
  if (y_stride <= 0 || u_stride <= 0 || v_stride <= 0) {
    return QImage();
  }
  // Refuse rather than read out of bounds: a truncated plane means the frame
  // is not actually I420, and painting garbage is worse than painting nothing.
  if (y_plane.size() < static_cast<size_t>(y_stride) * (height - 1) + width ||
      u_plane.size() <
          static_cast<size_t>(u_stride) * (chroma_height - 1) + chroma_width ||
      v_plane.size() <
          static_cast<size_t>(v_stride) * (chroma_height - 1) + chroma_width) {
    return QImage();
  }

  const YuvCoefficients& c = PickCoefficients(frame.color_space(), height);
  const bool full_range = frame.color_space().range == media::ColorRange::kFull;

  QImage image(width, height, QImage::Format_RGB32);
  if (image.isNull()) {
    return QImage();
  }
  auto* out = reinterpret_cast<QRgb*>(image.bits());

  for (int row = 0; row < height; ++row) {
    const uint8_t* y_row = y_plane.data() + static_cast<size_t>(row) * y_stride;
    const uint8_t* u_row =
        u_plane.data() + static_cast<size_t>(row / 2) * u_stride;
    const uint8_t* v_row =
        v_plane.data() + static_cast<size_t>(row / 2) * v_stride;
    QRgb* out_row = out + static_cast<size_t>(row) * width;
    for (int col = 0; col < width; ++col) {
      const int luma =
          full_range ? y_row[col] * 1024 : std::max(0, y_row[col] - 16) * 1192;
      const int u = static_cast<int>(u_row[col / 2]) - 128;
      const int v = static_cast<int>(v_row[col / 2]) - 128;
      const int r = Clamp8((luma + c.rv * v) >> 10);
      const int g = Clamp8((luma + c.gu * u + c.gv * v) >> 10);
      const int b = Clamp8((luma + c.bu * u) >> 10);
      out_row[col] = qRgb(r, g, b);
    }
  }
  return image;
}

// The frame's display rotation, applied once at conversion time rather than on
// every paint. Quarter turns only: a container's display matrix is always one,
// and anything else is a caller passing an angle nobody downstream honours.
QImage RotateImage(const QImage& image, int degrees) {
  int quarters = (degrees / 90) % 4;
  if (quarters < 0) {
    quarters += 4;
  }
  if (quarters == 0) {
    return image;
  }
  QTransform transform;
  transform.rotate(quarters * 90);
  return image.transformed(transform);
}

// Letterbox: fit |source| inside |box| keeping aspect ratio, centred.
QRectF FitRect(const QSizeF& source, const QRectF& box) {
  if (source.isEmpty() || box.isEmpty()) {
    return box;
  }
  const qreal scale =
      std::min(box.width() / source.width(), box.height() / source.height());
  const QSizeF scaled(source.width() * scale, source.height() * scale);
  return QRectF(box.center().x() - scaled.width() / 2.0,
                box.center().y() - scaled.height() / 2.0, scaled.width(),
                scaled.height());
}

}  // namespace

VideoItem::VideoItem(QQuickItem* parent) : QQuickPaintedItem(parent) {
  // The frame arrives on avbase's render thread; this item only ever reads it
  // from the GUI thread, one mailbox swap per tick.
  timer_ = new QTimer(this);
  timer_->setInterval(16);
  timer_->setTimerType(Qt::PreciseTimer);
  QObject::connect(timer_, &QTimer::timeout, this, &VideoItem::refresh);
  timer_->start();
}

VideoItem::~VideoItem() {
  // The comparison the GPU item exists to justify, printed only when asked:
  // this number is CPU work that the shader path does on the GPU instead.
  if (converted_frames_ > 0 && qEnvironmentVariableIsSet("AVBASE_QT_TIMING")) {
    std::fprintf(stderr, "[qt-cpu] frames=%d convert_avg=%.3fms\n",
                 converted_frames_, convert_ms_ / converted_frames_);
  }
}

void VideoItem::setMailbox(FrameMailbox* mailbox) {
  mailbox_ = mailbox;
  last_serial_ = -1;
}

void VideoItem::refresh() {
  if (mailbox_ == nullptr) {
    return;
  }
  int64_t serial = 0;
  base::scoped_refptr<media::VideoFrame> frame = mailbox_->Load(&serial);
  if (!frame || serial == last_serial_) {
    return;
  }
  last_serial_ = serial;

  // Diagnostic switch: consume the frame but never touch its pixels. Used to
  // tell "our conversion corrupts memory" apart from "the decoder already
  // did". Costs one env lookup per frame, which is why it is not in the
  // hot path unless set.
  if (qEnvironmentVariableIsSet("AVBASE_QT_NO_PIXELS")) {
    return;
  }

  // A hardware frame has no readable planes; ToI420() is the explicit readback
  // the zero-copy contract documents. It returns the same frame when the
  // pixels are already CPU-side, so this is cheap on the software path.
  base::scoped_refptr<media::VideoFrame> readable = frame;
  if (!frame->IsMappable()) {
    readable = frame->ToI420();
    if (!readable) {
      // No producer-side readback installed. Say so once per frame rather than
      // painting green: "the hardware path is on and nothing can read it back"
      // is a configuration answer, not a rendering bug.
      frame_info_ =
          QStringLiteral("opaque frame (%1), no readback")
              .arg(media::GetNativeHandleKindName(frame->native_handle().kind));
      emit frameChanged();
      return;
    }
  }

  const auto convert_begin = std::chrono::steady_clock::now();
  QImage image = I420ToImage(*readable);
  if (qEnvironmentVariableIsSet("AVBASE_QT_TIMING")) {
    convert_ms_ += std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - convert_begin)
                       .count();
    ++converted_frames_;
  }
  if (image.isNull()) {
    // Not every decoded format is I420 (NV12 from a hardware path, 10-bit
    // sources). Painting the previous frame forever would look like a hang, so
    // surface the format instead.
    frame_info_ = QStringLiteral("unsupported format %1")
                      .arg(media::GetVideoFormatName(readable->format()));
    emit frameChanged();
    return;
  }

  image_ = RotateImage(image, readable->rotation());
  frame_info_ =
      QStringLiteral("%1x%2").arg(image_.width()).arg(image_.height());
  emit frameChanged();
  update();
}

void VideoItem::paint(QPainter* painter) {
  if (image_.isNull()) {
    // Background only. The "no signal" prompt belongs to whoever knows WHY
    // there is no frame -- QML knows whether a source was ever opened, this
    // item only knows it has no pixels. Painting a message here too is how the
    // two texts ended up stacked on top of each other.
    painter->fillRect(boundingRect(), QColor(12, 12, 14));
    return;
  }
  painter->fillRect(boundingRect(), QColor(0, 0, 0));
  painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter->drawImage(FitRect(QSizeF(image_.size()), boundingRect()), image_);
}

}  // namespace avbase::qt
