// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_yuv_colour.h"

#include "media/base/video_color_space.h"

namespace avbase::media {

constexpr YuvMatrix kMatrix601Limited = {{1.164383f, 1.164383f, 1.164383f, 0.0f,
                                          -0.391762f, 2.017232f, 1.596027f,
                                          -0.812968f, 0.0f},
                                         16.0f / 255.0f};
constexpr YuvMatrix kMatrix601Full = {
    {1.0f, 1.0f, 1.0f, 0.0f, -0.344136f, 1.772f, 1.402f, -0.714136f, 0.0f},
    0.0f};
constexpr YuvMatrix kMatrix709Limited = {{1.164383f, 1.164383f, 1.164383f, 0.0f,
                                          -0.213249f, 2.112402f, 1.792741f,
                                          -0.532909f, 0.0f},
                                         16.0f / 255.0f};
constexpr YuvMatrix kMatrix709Full = {
    {1.0f, 1.0f, 1.0f, 0.0f, -0.187324f, 1.8556f, 1.5748f, -0.468124f, 0.0f},
    0.0f};
// BT.2020 non-constant-luminance, limited range. The chroma coefficients are
// the FULL-range ones times 255/224 (= 1.138393), the same relationship the two
// BT.709 rows above have: 0.164553*1.138393 = 0.187326 and 0.571353*1.138393 =
// 0.650424. Copying the full-range row here instead (as this table briefly did)
// leaves the limited-range Y scaling and offset in place while the chroma is
// wrong, which is an off-by-a-matrix colour error rather than a visible crash:
// limited-range BT.2020 content renders with the wrong saturation.
constexpr YuvMatrix kMatrix2020Limited = {{1.164383f, 1.164383f, 1.164383f,
                                           0.0f, -0.187326f, 2.141772f,
                                           1.678673f, -0.650424f, 0.0f},
                                          16.0f / 255.0f};
constexpr YuvMatrix kMatrix2020Full = {
    {1.0f, 1.0f, 1.0f, 0.0f, -0.164553f, 1.8814f, 1.4746f, -0.571353f, 0.0f},
    0.0f};

YuvMatrix SelectYuvMatrix(const VideoColorSpace& cs) {
  const bool full = cs.range == ColorRange::kFull;
  switch (cs.matrix) {
  case ColorMatrix::kBT709:
    return full ? kMatrix709Full : kMatrix709Limited;
  case ColorMatrix::kBT2020Ncl:
  case ColorMatrix::kBT2020Cl:
    return full ? kMatrix2020Full : kMatrix2020Limited;
  // Same body as `default` below, and deliberately not merged into it: naming
  // kSMPTE170M here is the documentation that SMPTE 170M maps to BT.601,
  // which a fallthrough into the unmarked default would not express.
  // NOLINTNEXTLINE
  case ColorMatrix::kSMPTE170M:
    return full ? kMatrix601Full : kMatrix601Limited;
  default:
    // Unmarked: sws and libyuv both assume limited-range BT.601; matching
    // them keeps the GL path indistinguishable from the CPU fallback.
    return full ? kMatrix601Full : kMatrix601Limited;
  }
}

}  // namespace avbase::media
