// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_PUBLIC_PLAYER_EXPORT_H_
#define AVBASE_PLAYER_PUBLIC_PLAYER_EXPORT_H_

#if defined(AVBASE_PLAYER_IMPLEMENTATION)
#define AVBASE_PLAYER_EXPORT __attribute__((visibility("default")))
#else
#define AVBASE_PLAYER_EXPORT
#endif

#endif  // AVBASE_PLAYER_PUBLIC_PLAYER_EXPORT_H_
