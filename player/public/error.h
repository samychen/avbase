// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Re-export header. The types live in media/base/media_error.h because the
// media layer reports through them and media/ must not depend on player/
// (invariant C22). SDK callers keep spelling them ijkpp::MediaError etc.

#ifndef IJKPP_PLAYER_PUBLIC_ERROR_H_
#define IJKPP_PLAYER_PUBLIC_ERROR_H_

#include "media/base/media_error.h"
#include "player/public/player_export.h"

namespace ijkpp {

using media::ErrorCode;
using media::GetErrorCodeName;
using media::MediaError;
using media::OkStatus;
using media::Result;
using media::Status;

// Convenience re-exports of the error constructors so call sites in player/
// and in host code read the same.
using media::Err;

}  // namespace ijkpp

#endif  // IJKPP_PLAYER_PUBLIC_ERROR_H_
