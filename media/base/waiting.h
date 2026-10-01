// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/waiting.h` (BSD-3-Clause).

#ifndef AVBASE_MEDIA_BASE_WAITING_H_
#define AVBASE_MEDIA_BASE_WAITING_H_

#include "base/functional/callback.h"
#include "media/media_export.h"

namespace avbase::media {

// Why a decoder or renderer is stalled. Kept in its own header because both
// VideoDecoder and AudioDecoder report it, and neither should depend on the
// other's header for the type.
enum class WaitingReason { kNone = 0, kKey, kDecoderStalled, kNoFreeSurface };

using WaitingCB = base::RepeatingCallback<void(WaitingReason)>;

AVBASE_MEDIA_EXPORT const char* GetWaitingReasonName(WaitingReason reason);

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_WAITING_H_
