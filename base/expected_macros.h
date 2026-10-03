// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/expected_macros.h` (BSD-3-Clause).

#ifndef AVBASE_BASE_EXPECTED_MACROS_H_
#define AVBASE_BASE_EXPECTED_MACROS_H_

#include <utility>

#include "base/types/expected.h"

// Evaluates |rexpr|. If it is unexpected, returns the error from the enclosing
// function, which must return base::expected<_, E>.
#define RETURN_IF_ERROR(rexpr) \
  AVBASE_RETURN_IF_ERROR_IMPL(AVBASE_CONCAT_(_avbase_err_, __LINE__), rexpr)

#define AVBASE_RETURN_IF_ERROR_IMPL(name, rexpr)                \
  decltype(auto) name = (rexpr);                                \
  if (!name.has_value()) {                                      \
    return ::avbase::base::unexpected(std::move(name).error()); \
  }

// Assigns the value of |rexpr| to |lhs|, returning the error on failure.
#define ASSIGN_OR_RETURN(lhs, rexpr)                                        \
  AVBASE_ASSIGN_OR_RETURN_IMPL(AVBASE_CONCAT_(_avbase_expected_, __LINE__), \
                               lhs, rexpr)

#define AVBASE_ASSIGN_OR_RETURN_IMPL(name, lhs, rexpr)          \
  auto&& name = (rexpr);                                        \
  if (!name.has_value()) {                                      \
    return ::avbase::base::unexpected(std::move(name).error()); \
  }                                                             \
  lhs = std::move(name).value()

#define AVBASE_CONCAT_INNER_(a, b) a##b
#define AVBASE_CONCAT_(a, b) AVBASE_CONCAT_INNER_(a, b)

#endif  // AVBASE_BASE_EXPECTED_MACROS_H_
