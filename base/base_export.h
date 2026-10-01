// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_BASE_BASE_EXPORT_H_
#define AVBASE_BASE_BASE_EXPORT_H_

#if defined(AVBASE_BASE_IMPLEMENTATION)
#define AVBASE_BASE_EXPORT __attribute__((visibility("default")))
#else
#define AVBASE_BASE_EXPORT
#endif

#endif  // AVBASE_BASE_BASE_EXPORT_H_
