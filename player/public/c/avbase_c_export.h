// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Export macro for the C ABI (docs/08 M15).
//
// Deliberately shaped like player/public/player_export.h rather than the usual
// MSVC/GCC two-branch dance: this tree names the visibility through
// AVBASE_PLAYER_IMPLEMENTATION (set by CMake on the target that defines the
// symbols) instead of sniffing the host with _WIN32. Two reasons:
//   * the C header has to stay includable by a C compiler on every host, and a
//     __declspec branch is one more thing that can differ between the host that
//     built the library and the host that consumes it, and
//   * invariant C5 keeps platform macros out of the portable layers, and
//     player/public/c/ is one of them.
//
// Consumers include only avbase_c.h and get the empty expansion, which is
// exactly what they want: the declaration, not an export promise.

#ifndef AVBASE_PLAYER_PUBLIC_C_AVBASE_C_EXPORT_H_
#define AVBASE_PLAYER_PUBLIC_C_AVBASE_C_EXPORT_H_

#if defined(AVBASE_C_IMPLEMENTATION)
#define AVBASE_C_API __attribute__((visibility("default")))
#else
#define AVBASE_C_API
#endif

#endif  // AVBASE_PLAYER_PUBLIC_C_AVBASE_C_EXPORT_H_
