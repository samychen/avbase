// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_MEDIA_EXPORT_H_
#define AVBASE_MEDIA_MEDIA_EXPORT_H_

#if defined(AVBASE_MEDIA_IMPLEMENTATION)
#define AVBASE_MEDIA_EXPORT __attribute__((visibility("default")))
#else
#define AVBASE_MEDIA_EXPORT
#endif

#endif  // AVBASE_MEDIA_MEDIA_EXPORT_H_
