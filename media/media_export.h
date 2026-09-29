// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_MEDIA_MEDIA_EXPORT_H_
#define IJKPP_MEDIA_MEDIA_EXPORT_H_

#if defined(IJKPP_MEDIA_IMPLEMENTATION)
#define IJKPP_MEDIA_EXPORT __attribute__((visibility("default")))
#else
#define IJKPP_MEDIA_EXPORT
#endif

#endif  // IJKPP_MEDIA_MEDIA_EXPORT_H_
