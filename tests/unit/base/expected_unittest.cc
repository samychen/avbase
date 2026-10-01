// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/types/expected.h"

#include <string>

#include "base/expected_macros.h"
#include "gtest/gtest.h"

namespace avbase::base {
namespace {

struct Error {
  int code{0};
  std::string message;
};

expected<int, Error> ParseInt(const std::string& s) {
  if (s == "42") {
    return 42;
  }
  return unexpected(Error{1, "not 42: " + s});
}

expected<void, Error> RequirePositive(int v) {
  if (v < 0) {
    return unexpected(Error{2, "negative"});
  }
  return {};
}

expected<int, Error> Chained(const std::string& s) {
  ASSIGN_OR_RETURN(const int value, ParseInt(s));
  RETURN_IF_ERROR(RequirePositive(value));
  return value * 2;
}

TEST(ExpectedTest, ValueCase) {
  const auto r = ParseInt("42");
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(static_cast<bool>(r));
  EXPECT_EQ(*r, 42);
  EXPECT_EQ(r.value(), 42);
}

TEST(ExpectedTest, ErrorCase) {
  const auto r = ParseInt("nope");
  ASSERT_FALSE(r.has_value());
  EXPECT_FALSE(static_cast<bool>(r));
  EXPECT_EQ(r.error().code, 1);
  EXPECT_EQ(r.error().message, "not 42: nope");
}

TEST(ExpectedTest, VoidSpecialization) {
  const expected<void, Error> ok_result = RequirePositive(1);
  EXPECT_TRUE(ok_result.has_value());

  const expected<void, Error> err_result = RequirePositive(-1);
  EXPECT_FALSE(err_result.has_value());
  EXPECT_EQ(err_result.error().code, 2);
}

TEST(ExpectedTest, MacrosPropagateErrors) {
  const auto good = Chained("42");
  ASSERT_TRUE(good.has_value());
  EXPECT_EQ(*good, 84);

  const auto bad = Chained("nope");
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code, 1);   // The original error survives propagation.
}

TEST(ExpectedTest, ExplicitConstructionAvoidsBoolAmbiguity) {
  // expected<bool, long> would silently accept 123L as a *value* under the
  // standard's implicit conversion rules. Constructing explicitly from the
  // intended type is the guard, since avbase does not ship base::ok (see the
  // note in base/types/expected.h).
  // A named bool rather than static_cast<bool>(true): the literal `true` is
  // already a bool, so that cast is a no-op that -Wuseless-cast (debug preset,
  // -Werror) rejects. The point being tested is that construction binds to the
  // bool alternative and not to `long`, which a named bool expresses just as
  // well -- and an integer literal such as 1 would still be rejected here.
  const bool value = true;
  const expected<bool, long> e(value);
  ASSERT_TRUE(e.has_value());
  EXPECT_TRUE(*e);
}

TEST(ExpectedTest, MoveSemantics) {
  auto r = ParseInt("42");
  expected<int, Error> moved = std::move(r);
  EXPECT_TRUE(moved.has_value());
  EXPECT_EQ(*moved, 42);
}

}  // namespace
}  // namespace avbase::base
