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
  EXPECT_EQ(bad.error().code, 1);  // The original error survives propagation.
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

// H5: a non-default-constructible T must work at all. The old union member
// initialiser (`Storage() : value()`) made *every* expected<T, E> require
// T() to compile, even ones that never default-construct.
struct NoDefault {
  explicit NoDefault(int v) : value(v) {}
  int value;
};

TEST(ExpectedTest, NonDefaultConstructibleValueWorks) {
  expected<NoDefault, Error> e(NoDefault(7));
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->value, 7);

  expected<NoDefault, Error> copy = e;
  EXPECT_EQ(copy->value, 7);

  expected<NoDefault, Error> moved = std::move(copy);
  EXPECT_EQ(moved->value, 7);

  e = expected<NoDefault, Error>(NoDefault(9));
  EXPECT_EQ(e->value, 9);

  e = expected<NoDefault, Error>(unexpected(Error{3, "switch to error"}));
  ASSERT_FALSE(e.has_value());
  EXPECT_EQ(e.error().code, 3);
}

// H5: exactly one T is built per value-holding expected and exactly one is
// destroyed per dying one. The old double construction leaked one T per
// default-constructed expected, unbalancing this count.
struct Counted {
  static inline int constructed = 0;
  static inline int destroyed = 0;
  Counted() { ++constructed; }
  Counted(const Counted&) { ++constructed; }
  Counted(Counted&&) { ++constructed; }
  Counted& operator=(const Counted&) = default;
  Counted& operator=(Counted&&) = default;
  ~Counted() { ++destroyed; }
};

TEST(ExpectedTest, ConstructionAndDestructionAreBalanced) {
  Counted::constructed = 0;
  Counted::destroyed = 0;
  {
    expected<Counted, Error> a;                               // default
    expected<Counted, Error> b(a);                            // copy
    expected<Counted, Error> c(std::move(b));                 // move
    expected<Counted, Error> d(unexpected(Error{}));          // error only
    d = a;                                                    // error -> value
    c = expected<Counted, Error>(unexpected(Error{}));        // value -> error
  }
  EXPECT_EQ(Counted::constructed, Counted::destroyed)
      << "every construction must be matched by exactly one destruction";
}

}  // namespace
}  // namespace avbase::base
