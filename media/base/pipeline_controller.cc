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
// implements (media/base/pipeline_controller.h): neither has been compiled in
// the environment that wrote them. To finish bringing this pair in, add the .cc
// to media/CMakeLists.txt and drop the DRAFT banner from both files in the same
// change.

#include "media/base/pipeline_controller.h"

namespace avbase::media {

const char* PipelineControllerStateToString(PipelineController::State state) {
  switch (state) {
    case PipelineController::State::kCreated:    return "created";
    case PipelineController::State::kStarting:   return "starting";
    case PipelineController::State::kReady:      return "ready";
    case PipelineController::State::kStopping:   return "stopping";
    case PipelineController::State::kStopped:    return "stopped";
    case PipelineController::State::kDestroying: return "destroying";
  }
  return "invalid";
}

PipelineController::PipelineController() = default;

// NOTE -- the header's ".cc owed by this header" list says this destructor must
// drive the state to kDestroying and join every sequence in the fixed order at
// docs/03 §10.1. That list was wrong about which class owes it: this class is
// abstract and holds no members (deviation 1 in pipeline_controller.h), so
// there is nothing here to drive and nothing to join. The ordering it describes
// is a property of the concrete implementation --
// media/filters/pipeline_impl.cc, which owns the sequences, the Demuxer and the
// Renderer -- and that is where mitigation 1 for risk R5 has to live. Left here
// rather than deleted so the next reader does not re-derive the same wrong
// conclusion from the header.
PipelineController::~PipelineController() = default;

}  // namespace avbase::media
