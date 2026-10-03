// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_BASE_MEMORY_PTR_UTIL_H_
#define AVBASE_BASE_MEMORY_PTR_UTIL_H_

#include <memory>
#include <utility>

namespace avbase::base {

template <typename T, typename... Args>
std::unique_ptr<T> WrapUnique(T* ptr) {
  return std::unique_ptr<T>(ptr);
}

// std::make_unique is preferred everywhere else; WrapUnique exists only for
// the cases where ownership arrives as a raw pointer from a C-style factory.
//
// There is deliberately no ref-counted counterpart here. MakeRefCounted,
// WrapRefCounted and AdoptRef live at the bottom of
// base/memory/scoped_refptr.h, because two of them need scoped_refptr's
// adopting constructor and this header does not (and should not) include it. If
// you came here looking for WrapRefCounted, that is the sign the naming split
// is working: unique ownership and shared ownership are different questions and
// base/ keeps their helpers in different places, matching Chromium.

}  // namespace avbase::base

#endif  // AVBASE_BASE_MEMORY_PTR_UTIL_H_
