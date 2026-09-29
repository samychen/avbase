// Copyright 2026 The ijkpp Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IJKPP_BASE_MEMORY_PTR_UTIL_H_
#define IJKPP_BASE_MEMORY_PTR_UTIL_H_

#include <memory>
#include <utility>

namespace ijkpp::base {

template <typename T, typename... Args>
std::unique_ptr<T> WrapUnique(T* ptr) {
  return std::unique_ptr<T>(ptr);
}

// std::make_unique is preferred everywhere else; WrapUnique exists only for
// the cases where ownership arrives as a raw pointer from a C-style factory.

}  // namespace ijkpp::base

#endif  // IJKPP_BASE_MEMORY_PTR_UTIL_H_
