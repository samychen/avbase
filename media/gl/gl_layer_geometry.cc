// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_layer_geometry.h"

#include <cmath>

#include "media/base/video_frame.h"

namespace avbase::media {
namespace {

// (cos, sin) of each CLOCKWISE quarter turn, in the order 0, 90, 180, 270.
// Four exact answers; see the header on why they are not computed.
constexpr float kCosSin[8] = {1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, -1.0f};

}  // namespace

int SnapLayerRotation(int degrees) {
  // Normalise to 0..359 first (C++ % truncates toward zero), then snap.
  int quarters = degrees % 360;
  if (quarters < 0) {
    quarters += 360;
  }
  return ((quarters + 45) / 90 % 4) * 90;
}

void LayerRotationToCosSin(int degrees, float* out_cos_sin) {
  const int quarter = SnapLayerRotation(degrees) / 90;
  out_cos_sin[0] = kCosSin[2 * quarter];
  out_cos_sin[1] = kCosSin[2 * quarter + 1];
}

void NormalisedRectToUvRect(float x, float y, float w, float h,
                            float* out_rect) {
  // NDC x spans -1..1 as the normalised x spans 0..1, so a normalised width of
  // w covers 2w of NDC and the half-extent is w. NDC y is flipped: normalised
  // y = 0 is the TOP, which in NDC is +1.
  out_rect[0] = 2.0f * (x + w * 0.5f) - 1.0f;
  out_rect[1] = 1.0f - 2.0f * (y + h * 0.5f);
  out_rect[2] = w;
  out_rect[3] = h;
}

float SourceAspectOf(const VideoFrame& frame) {
  const Size rotated = frame.NaturalSizeForRotation();
  if (rotated.width <= 0 || rotated.height <= 0) {
    return 0.0f;
  }
  const Rational sar = frame.sar();
  // A zero or malformed sar is legal in a container and has to mean "square",
  // not "infinitely wide".
  const double sample = (sar.is_zero() || sar.den == 0) ? 1.0 : sar.ToDouble();
  if (!(sample > 0.0)) {
    return 0.0f;
  }
  // A quarter turn swaps the axes, and with them which side the stored
  // sample's own width lands on. |rotated| is already transposed, so undoing
  // that is what recovers the stored grid.
  const int quarter = SnapLayerRotation(frame.rotation()) / 90;
  const bool transposed = (quarter % 2) == 1;
  const double stored_w =
      static_cast<double>(transposed ? rotated.height : rotated.width);
  const double stored_h =
      static_cast<double>(transposed ? rotated.width : rotated.height);
  return static_cast<float>((stored_w * sample) / stored_h);
}

}  // namespace avbase::media
