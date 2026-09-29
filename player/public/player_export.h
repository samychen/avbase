// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_PLAYER_PUBLIC_PLAYER_EXPORT_H_
#define IJKPP_PLAYER_PUBLIC_PLAYER_EXPORT_H_

#if defined(IJKPP_PLAYER_IMPLEMENTATION)
#define IJKPP_PLAYER_EXPORT __attribute__((visibility("default")))
#else
#define IJKPP_PLAYER_EXPORT
#endif

#endif  // IJKPP_PLAYER_PUBLIC_PLAYER_EXPORT_H_
