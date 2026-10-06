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
// implements (media/base/pipeline.h): neither has been compiled in the
// environment that wrote them. To finish bringing this pair in, add the .cc to
// media/CMakeLists.txt and drop the DRAFT banner from both files in the same
// change.

#include "media/base/pipeline.h"

#include "media/base/native_display.h"

namespace avbase::media {

// Out-of-line `= default`, same convention and same reason as
// media/base/demuxer.cc: this translation unit becomes the key function for
// Pipeline's vtable, so it is emitted once rather than in every includer.
// Pipeline::Client needs nothing -- every member is pure virtual and its
// destructor is inline-defaulted.
// Out-of-line for the same reason as Renderer::SetOutputTarget: the
// scoped_refptr destructor needs NativeDisplay complete.
void Pipeline::SetOutputTarget(base::scoped_refptr<NativeDisplay> display) {
  (void)display;
}

void Pipeline::TakeSnapshot(base::TimeDelta at,
                            Renderer::SnapshotFrameCallback callback) {
  (void)at;
  std::move(callback).Run(
      MediaError(ErrorCode::kNotImplemented,
                 "this pipeline does not support snapshots", {}, {}),
      nullptr);
}

Pipeline::Pipeline() = default;
Pipeline::~Pipeline() = default;

}  // namespace avbase::media
