// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
//
// STATUS: IN THE BUILD (promoted from DRAFT, tenth round)
// (promoted from DRAFT, tenth round), for the same reason as the header it
// PROMOTED into the build in the tenth round, after a full compile and test
// run (docs/PROGRESS.md §3l). Any wording below saying this file has never
// been compiled, or is excluded from every CMake target, is HISTORICAL: it
// describes the state when the file was written and is kept so the reasoning
// behind each gap list stays readable. The gap lists themselves are still
// open unless a later note says otherwise.
// implements (media/base/renderer_client.h): neither has been compiled in the
// environment that wrote them. To finish bringing this pair in, add the .cc to
// media/CMakeLists.txt and drop the DRAFT banner from both files in the same
// change.

#include "media/base/renderer_client.h"

namespace ijkpp::media {

const char* BufferingStateToString(BufferingState state) {
  switch (state) {
    case BufferingState::kHaveNothing:  return "have-nothing";
    case BufferingState::kHaveMetadata: return "have-metadata";
    case BufferingState::kHaveEnough:   return "have-enough";
    case BufferingState::kHaveFuture:   return "have-future";
  }
  return "invalid";
}

const char* OutputDeviceStatusToString(OutputDeviceStatus status) {
  switch (status) {
    case OutputDeviceStatus::kOk:           return "ok";
    case OutputDeviceStatus::kFailed:       return "failed";
    case OutputDeviceStatus::kNotAuthorized:
      return "not-authorized";
    case OutputDeviceStatus::kNotFound:     return "not-found";
  }
  return "invalid";
}

}  // namespace ijkpp::media
