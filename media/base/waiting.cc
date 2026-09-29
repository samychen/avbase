// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/waiting.h"

namespace ijkpp::media {

const char* GetWaitingReasonName(WaitingReason reason) {
  switch (reason) {
    case WaitingReason::kNone: return "none";
    case WaitingReason::kKey: return "key";
    case WaitingReason::kDecoderStalled: return "decoder-stalled";
    case WaitingReason::kNoFreeSurface: return "no-free-surface";
  }
  return "unknown";
}

}  // namespace ijkpp::media
