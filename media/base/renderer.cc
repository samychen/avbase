// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// (promoted from DRAFT, tenth round), for the same reason as the header it
// implements (media/base/renderer.h): neither has been compiled in the
// environment that wrote them. To finish bringing this pair in, add the .cc to
// media/CMakeLists.txt and drop the DRAFT banner from both files in the same
// change.

#include "media/base/renderer.h"

#include <utility>

#include "base/logging.h"
#include "media/base/native_display.h"

namespace avbase::media {

const char* RendererTypeToString(RendererType type) {
  switch (type) {
  case RendererType::kRendererImpl:
    return "RendererImpl";
  case RendererType::kNullRenderer:
    return "NullRenderer";
  case RendererType::kCastRenderer:
    return "CastRenderer";
  }
  return "invalid";
}

// Out-of-line `= default`, matching media/base/demuxer.cc and
// media/base/video_decoder.cc. The point is not stylistic: with the destructor
// defined here, this translation unit is the key function for Renderer's
// vtable, so the vtable is emitted once instead of in every includer.
// The default body lives here rather than inline in the header: |display|'s
// scoped_refptr destructor needs the complete type, and the forward
// declaration in the header keeps every includer's closure small.
void Renderer::SetOutputTarget(base::scoped_refptr<NativeDisplay> display) {
  (void)display;
}

void Renderer::TakeSnapshot(base::TimeDelta at,
                            SnapshotFrameCallback callback) {
  std::move(callback).Run(
      MediaError(ErrorCode::kNotImplemented,
                 "this renderer holds no snapshot-able video leg",
                 "TakeSnapshot on a renderer built without a video stream",
                 "only request snapshots while a video track is selected"),
      nullptr);
}

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

void Renderer::SetCdm(CdmContext* cdm_context,
                      base::OnceCallback<void(bool)> cdm_attached_cb) {
  // Decision D8: DRM is not implemented, and cdm_context.h deliberately does
  // not exist so that the empty interface cannot be mistaken for an API
  // promise. The callback MUST still run, with false: a caller awaiting it
  // would otherwise block forever, which is the failure class Δ15 exists to
  // prevent ("leak a thread rather than hang the caller" -- and here there is
  // not even a thread to leak, just a promise never kept).
  (void)cdm_context;
  LOG(WARNING) << "avbase.pipeline: SetCdm() ignored; DRM is not implemented "
                  "in this build (decision D8)";
  if (cdm_attached_cb) {
    std::move(cdm_attached_cb).Run(false);
  }
}

}  // namespace avbase::media
