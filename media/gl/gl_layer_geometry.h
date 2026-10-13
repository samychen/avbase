// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The geometry of a placed layer: the angle it is drawn at, the box it is
// drawn into, and the shape of the picture inside it.
//
// These are PURE functions of a frame or an angle -- no GL objects, no
// context, no layer state -- which is why they are free functions rather than
// members. They are also SHARED: the scene that places layers, the presenter
// that draws one, the layer that uploads, and the tests that pin the
// conventions all ask the same questions and have to get the same answers.
// One definition each, then -- the point of a convention is that it cannot be
// spelled two ways.

#ifndef AVBASE_MEDIA_GL_GL_LAYER_GEOMETRY_H_
#define AVBASE_MEDIA_GL_GL_LAYER_GEOMETRY_H_

#include "media/base/video_frame.h"

namespace avbase::media {

// Snaps |degrees| to the nearest quarter turn and normalises it into 0, 90,
// 180, 270. Exported because both the setter that records an angle and the
// shader-facing conversion need the same answer, and "what did that 89 become"
// is a question with exactly one right reply.
AVBASE_MEDIA_EXPORT int SnapLayerRotation(int degrees);

// (cos, sin) of a CLOCKWISE turn of |degrees| about a layer's own centre, for
// the uRotation uniform. Four values are possible because a container's display
// rotation is a quarter-turn count and nothing downstream can honour an
// arbitrary angle -- so the input is snapped rather than rejected and then
// silently mishandled.
//
// A table and not std::cos/std::sin: the four answers a caller can reach are
// exactly representable, and the library's are not. Computing cos(90 degrees)
// yields ~6e-17, which is a sub-pixel sampling offset -- invisible in a still
// and wrong in a rotation test that compares two orientations of one picture.
AVBASE_MEDIA_EXPORT void LayerRotationToCosSin(int degrees, float* out_cos_sin);

// Turns a rect in NORMALISED target coordinates -- (0,0) top-left, (1,1)
// bottom-right, |w|/|h| as fractions of the target -- into the uRect the rect
// vertex stage takes: xy is the centre in NDC and zw the half-extent, so a
// full target is (0, 0, 1, 1).
//
// This is NOT the spelling gl::TextOverlay uses (there x/y are already NDC
// offsets and w/h are fractions of the half-extent), and the difference is
// worth a function rather than a comment at each call site: the two agree at
// the full-target case and diverge everywhere else.
AVBASE_MEDIA_EXPORT void NormalisedRectToUvRect(float x, float y, float w,
                                                float h, float* out_rect);

// The WIDTH OVER HEIGHT the picture is meant to be SEEN at: its natural size
// turned by the container's display rotation, with the sample aspect ratio
// folded in. Only the ratio is meaningful -- these are proportions, not a size
// anything gets allocated at.
//
// The sar belongs to the STORED sample grid and scales it along its width, so a
// quarter turn does not cancel it -- it MOVES it onto the other axis. 720x576
// PAL is 16/15 wider per sample whether or not the clip is then turned on its
// side; a caller that forgot the sar here would letterbox it 15% too narrow.
//
// Zero means "no shape to speak of" (no picture, or a degenerate one), which is
// the caller's signal not to fit.
AVBASE_MEDIA_EXPORT float SourceAspectOf(const VideoFrame& frame);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_GL_GL_LAYER_GEOMETRY_H_
