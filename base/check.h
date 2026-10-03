// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/check.h` and `base/check_op.h`
// (BSD-3-Clause, Copyright The Chromium Authors).
//
// avbase is built with -fno-exceptions, so a failed check aborts the process
// after logging, exactly like Chromium.

#ifndef AVBASE_BASE_CHECK_H_
#define AVBASE_BASE_CHECK_H_

#include <cstdlib>
#include <sstream>

#include "base/logging.h"

// CHECK() is active in all build configurations. Use it for conditions that
// must never be false, including on untrusted input that has already been
// validated by an earlier layer.
#define CHECK(condition)                                       \
  ::avbase::base::internal::CheckOpStreamHelper(!!(condition)) \
      << "Check failed: " #condition ". "

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NE(a, b) CHECK((a) != (b))
#define CHECK_LT(a, b) CHECK((a) < (b))
#define CHECK_LE(a, b) CHECK((a) <= (b))
#define CHECK_GT(a, b) CHECK((a) > (b))
#define CHECK_GE(a, b) CHECK((a) >= (b))

// DCHECK() is only active when AVBASE_ENABLE_DCHECK is defined.
#if defined(AVBASE_ENABLE_DCHECK)
#define DCHECK(condition) CHECK(condition)
#define DCHECK_EQ(a, b) CHECK_EQ(a, b)
#define DCHECK_NE(a, b) CHECK_NE(a, b)
#define DCHECK_LT(a, b) CHECK_LT(a, b)
#define DCHECK_LE(a, b) CHECK_LE(a, b)
#define DCHECK_GT(a, b) CHECK_GT(a, b)
#define DCHECK_GE(a, b) CHECK_GE(a, b)
#else
#define DCHECK(condition) ::avbase::base::internal::NullCheckStream()
#define DCHECK_EQ(a, b) DCHECK(true)
#define DCHECK_NE(a, b) DCHECK(true)
#define DCHECK_LT(a, b) DCHECK(true)
#define DCHECK_LE(a, b) DCHECK(true)
#define DCHECK_GT(a, b) DCHECK(true)
#define DCHECK_GE(a, b) DCHECK(true)
#endif

#define NOTREACHED()                                   \
  ::avbase::base::internal::CheckOpStreamHelper(false) \
      << "NOTREACHED() hit at " << __FILE__ << ":" << __LINE__ << ". "

namespace avbase {
namespace base {
namespace internal {

// Streams the message, then aborts on destruction if |condition| was false.
class CheckOpStreamHelper {
 public:
  explicit CheckOpStreamHelper(bool condition) : condition_(condition) {}
  CheckOpStreamHelper(const CheckOpStreamHelper&) = delete;
  CheckOpStreamHelper& operator=(const CheckOpStreamHelper&) = delete;
  ~CheckOpStreamHelper();

  template <typename T>
  CheckOpStreamHelper& operator<<(const T& value) {
    if (!condition_) {
      stream_ << value;
    }
    return *this;
  }

 private:
  bool condition_;
  std::ostringstream stream_;
};

// Absorbs streamed values without doing anything (used by disabled DCHECKs).
class NullCheckStream {
 public:
  template <typename T>
  NullCheckStream& operator<<(const T&) {
    return *this;
  }
};

}  // namespace internal
}  // namespace base
}  // namespace avbase

#endif  // AVBASE_BASE_CHECK_H_
