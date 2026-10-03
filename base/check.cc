// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/check.h"

#include <cstdlib>

namespace avbase::base::internal {

// CHECK/DCHECK failure path. avbase is built with -fno-exceptions, so a failed
// check logs at FATAL severity (which aborts) and then aborts unconditionally,
// matching Chromium's behaviour.
CheckOpStreamHelper::~CheckOpStreamHelper() {
  if (condition_) {
    return;
  }
  const std::string text = stream_.str();
  LOG(FATAL) << text;
  std::abort();  // Unreachable: LOG(FATAL) aborts. Keeps the compiler quiet.
}

}  // namespace avbase::base::internal
