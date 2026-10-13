// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The YUV -> RGB colour conversion, as tables.
//
// This is DATA, not behaviour: six constant matrices and the one function that
// picks between them. It is apart from gl_video_layer.cc because that file is
// otherwise entirely about texture and uniform state, and because a colour
// table is the kind of thing that has to be checkable on its own -- a wrong
// coefficient here is an off-by-a-matrix saturation error, which no return
// code and no GL error reports.
//
// It is not a full colour pipeline: no gamut conversion, no EOTF, no
// HDR -> SDR tone mapping. avbase has none of those; see docs/TODO 5.

#ifndef AVBASE_MEDIA_GL_GL_YUV_COLOUR_H_
#define AVBASE_MEDIA_GL_GL_YUV_COLOUR_H_

#include <array>

#include "media/base/video_color_space.h"
#include "media/media_export.h"

namespace avbase::media {

// Rows are (Y, U, V) -> (R, G, B); stored column-major for
// glUniformMatrix3fv (column 0 = the three Y coefficients, and so on).
struct YuvMatrix {
  std::array<float, 9> m;
  float offset_y;
};

// The conversion a frame's colour tags ask for: the matrix AND its luma offset
// together, because picking one without the other is how a picture ends up with
// the right saturation and the wrong black level.
//
// Unmarked content gets limited-range BT.601, matching sws_scale and libyuv, so
// the GL path is indistinguishable from the CPU fallback -- which is the only
// way a difference between the two would ever be noticed.
AVBASE_MEDIA_EXPORT YuvMatrix SelectYuvMatrix(const VideoColorSpace& cs);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_YUV_COLOUR_H_
