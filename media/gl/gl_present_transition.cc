// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The transition half of GlPresenter: two scenes in, one picture out.
//
// Split from gl_present.cc along a seam the code already had -- the
// transition's own state (two off-screen sides and the program that mixes
// them) is used by nothing else in the presenter, and the rest of the
// presenter is reachable without it. The two files are one class, so this is a
// cut by feature rather than by lifetime: nothing is constructed or destroyed
// across the boundary.

#include "media/gl/gl_present.h"

#include <string>

#include "base/logging.h"
#include "base/time/time.h"
#include "media/gl/gl_layer_geometry.h"
#include "render/gl/gl_framebuffer.h"
#include "render/gl/gl_transition.h"

namespace avbase::media {

bool GlPresenter::SetTransitionKind(gl::TransitionKind kind) {
  // Recorded only. The program is built by EnsureTransition(): compiling a
  // shader needs a current context, and a setter has no business assuming one
  // -- a caller may well decide the kind on a UI thread.
  //
  // Rejected when the kind is not one of the enum's named values. Casting an
  // arbitrary int would otherwise reach the switch in DirectionOf() and
  // silently fall through to the cross-fade, which is the kind of "works, but
  // not the way you asked" that a boolean return exists to prevent.
  switch (kind) {
  case gl::TransitionKind::kCrossFade:
  case gl::TransitionKind::kWipeRight:
  case gl::TransitionKind::kWipeLeft:
  case gl::TransitionKind::kWipeDown:
  case gl::TransitionKind::kWipeUp:
    break;
  default:
    return false;
  }
  transition_kind_ = kind;
  return true;
}

gl::GlTransition* GlPresenter::EnsureTransition() {
  if (transition_ && transition_->kind() == transition_kind_) {
    return transition_.get();
  }
  // A kind change throws the old program away here rather than in the setter,
  // for the reason the setter's comment gives: this runs on the render path,
  // where the context is current and a compile failure can be reported as a
  // failed compose instead of an ignored one.
  //
  // The old program is destroyed with the context current, which is what
  // Shutdown() also arranges and for the same reason: deleting GL objects
  // without one leaks them.
  std::string error;
  auto next = gl::GlTransition::Create(*api_, transition_kind_, &error);
  if (!next) {
    LOG(ERROR) << "gl present: transition: " << error;
    return nullptr;
  }
  transition_ = std::move(next);
  return transition_.get();
}

bool GlPresenter::ComposeTransition(GlScene& from, GlScene& to, float progress,
                                    PresentTiming* timing) {
  if (!gl_current_) {
    return false;
  }
  const base::TimeTicks t_begin =
      timing ? base::TimeTicks::Now() : base::TimeTicks();
  if (!EnsureCanvas()) {
    return false;
  }
  std::string error;
  if (!transition_from_->Ensure(canvas_->width(), canvas_->height(), &error) ||
      !transition_to_->Ensure(canvas_->width(), canvas_->height(), &error)) {
    LOG(ERROR) << "gl present: transition target: " << error;
    return false;
  }

  transition_from_->Bind();
  from.Compose(canvas_->width(), canvas_->height());
  transition_to_->Bind();
  to.Compose(canvas_->width(), canvas_->height());

  gl::GlTransition* transition = EnsureTransition();
  if (!transition) {
    return false;
  }

  // Back to the canvas for the mix. Its viewport matches the two sides (see
  // the header on why they are composed at the output size), so the transition
  // is a combination rather than a resample.
  canvas_->Bind();
  transition->Draw(transition_from_->color_texture(),
                   transition_to_->color_texture(), progress, canvas_->width(),
                   canvas_->height());
  DrawOverlay();
  if (timing) {
    timing->compose_ms = (base::TimeTicks::Now() - t_begin).InMillisecondsF();
  }
  return true;
}

bool GlPresenter::PresentTransition(GlScene& from, GlScene& to, float progress,
                                    PresentTiming* timing) {
  if (!ComposeTransition(from, to, progress, timing)) {
    return false;
  }
  return Swap(timing);
}

}  // namespace avbase::media
