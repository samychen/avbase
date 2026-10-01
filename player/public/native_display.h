// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Forwarding header: the type lives in media/base so that sinks can use it,
// but SDK users include it from here to keep the public surface in one place.

#ifndef AVBASE_PLAYER_PUBLIC_NATIVE_DISPLAY_H_
#define AVBASE_PLAYER_PUBLIC_NATIVE_DISPLAY_H_

#include "media/base/native_display.h"

namespace avbase {
using media::NativeDisplay;
using media::NativeDisplayKind;
using media::WaylandSurfaceHandle;
using media::X11WindowHandle;
}  // namespace avbase

#endif  // AVBASE_PLAYER_PUBLIC_NATIVE_DISPLAY_H_
