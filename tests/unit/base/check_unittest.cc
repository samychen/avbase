// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// base/check.h, and the three things about it that are easy to get wrong:
//
//   1. CHECK(x) is a temporary object whose destructor aborts, so the abort
//      lands at the semicolon rather than at the `<<` -- which is exactly
//      what makes `CHECK(x) << "context"` possible.
//   2. A failed check logs from base/check.cc:20, so the file:line the log
//      carries is check.cc's, not the caller's. NOTREACHED() streams
//      __FILE__ and __LINE__ into the text by hand for that reason.
//   3. A *disabled* DCHECK() still evaluates everything streamed into it,
//      because operator<<(const T&) binds its argument before the empty body
//      runs. Chromium's release DCHECK short-circuits the whole stream with
//      `true || (...)`; ours does not.

#include "base/check.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/logging.h"
#include "gtest/gtest.h"

namespace avbase::base {
namespace {

// Records what reaches the logging system so a test can assert that a
// *passing* check stays silent. Two properties of the delegate API matter
// here (base/logging.h:47-51): it is process-global, and installing a new
// delegate deletes the outgoing one -- so a test must copy its entries out
// before restoring, which is why each user below looks roundabout.
class RecordingDelegate : public logging::LoggingDelegate {
 public:
  void OnLogMessage(logging::LogSeverity, const char*, int,
                    std::string_view message) override {
    messages.emplace_back(message);
  }

  std::vector<std::string> messages;
};

// ---------------------------------------------------------------------------
// 1. 通过：不死，也不出声
// ---------------------------------------------------------------------------

// Reaching the end of this test IS the assertion. A failed check aborts the
// whole binary, so gtest never records a result for it.
TEST(CheckTest, PassingChecksRunToCompletion) {
  CHECK(true);
  CHECK_EQ(1, 1);
  CHECK_NE(1, 2);
  CHECK_LT(1, 2);
  CHECK_LE(1, 1);
  CHECK_GT(2, 1);
  CHECK_GE(1, 1);
  SUCCEED();
}

// The early return in ~CheckOpStreamHelper() is the contract: without it every
// CHECK would be a LOG(FATAL) and abort unconditionally.
TEST(CheckTest, PassingCheckLogsNothing) {
  auto* sink = new RecordingDelegate();
  logging::SetDelegate(std::unique_ptr<logging::LoggingDelegate>(sink));

  CHECK(true) << "must not appear";
  CHECK_EQ(2, 2) << "must not appear";

  // Copied before restoring: SetDelegate() deletes the outgoing delegate.
  const std::vector<std::string> messages = sink->messages;
  logging::SetDelegate(nullptr);

  EXPECT_TRUE(messages.empty());
}

// CHECK_EQ(a, b) -> CHECK((a) == (b)): each operand is written exactly once,
// so each is evaluated exactly once. Worth pinning, because the obvious
// alternative implementation -- a helper that formats both sides -- silently
// doubles any side effect in the arguments.
TEST(CheckTest, OperandsEvaluatedExactlyOnce) {
  int calls = 0;
  auto bump = [&calls] {
    ++calls;
    return 7;
  };
  CHECK_EQ(bump(), bump());
  EXPECT_EQ(calls, 2);
}

// ---------------------------------------------------------------------------
// 2. NullCheckStream：关闭的 DCHECK 落到这里
// ---------------------------------------------------------------------------

// It exists so `DCHECK(x) << foo;` still compiles with DCHECKs off -- erasing
// the macro would leave a bare `<< foo;` behind. Two contracts; the second is
// the one that bites.
TEST(NullCheckStreamTest, ChainReturnsItself) {
  internal::NullCheckStream stream;
  EXPECT_EQ(&(stream << 1), &stream);
  EXPECT_EQ(&(stream << 1 << std::string("x") << 2.5), &stream);
}

// Contract (2): the operands are STILL EVALUATED. Binding `const T&` evaluates
// the argument before the body runs, so `DCHECK(x) << Expensive()` costs you in
// a release build even though the check is gone. This is also why
// DCHECK_EQ(a, b) behaves differently from DCHECK(x) << y(): the former
// expands to DCHECK(true) and evaluates neither operand, the latter runs y().
TEST(NullCheckStreamTest, StreamedOperandsAreStillEvaluated) {
  int calls = 0;
  auto bump = [&calls] {
    ++calls;
    return 42;
  };
  internal::NullCheckStream stream;
  stream << bump();
  EXPECT_EQ(calls, 1);
}

// ---------------------------------------------------------------------------
// 3. DCHECK：两种构建两种语义，分开断言
// ---------------------------------------------------------------------------
//
// AVBASE_ENABLE_DCHECK is a compile definition from
// cmake/AvbaseCompilerFlags.cmake:58 that reaches test translation units too,
// so what DCHECK means is a property of the build, not of the call site. Both
// branches below are real code; only the one matching this configuration is
// compiled. The default is ON (cmake/AvbaseOptions.cmake:52); `release` is the
// one preset that turns it off.

#if defined(AVBASE_ENABLE_DCHECK)

// DCHECK is CHECK here -- there is no half form -- so a false one aborts and a
// true one must not.
TEST(DcheckTest, EnabledDcheckMatchesCheck) {
  DCHECK(true);
  DCHECK_EQ(3, 3);
  DCHECK_NE(3, 4);
  SUCCEED();
}

#else

// With DCHECKs off, DCHECK(cond) discards |cond| entirely (it expands to
// DCHECK(true)) but keeps the `<<` operands. Asserting both halves in one test
// is the point: the asymmetry is invisible unless you go looking for it.
TEST(DcheckTest, DisabledDcheckSkipsConditionButNotStream) {
  int condition_calls = 0;
  // [[maybe_unused]] is not decoration here -- it is the assertion. With
  // DCHECKs off, DCHECK(cond) expands to DCHECK(true) and drops |cond| from
  // the expansion entirely, so the compiler sees a variable nothing reads.
  // The attribute is only *needed* because the condition really is discarded,
  // and it only *compiles* for that reason too.
  [[maybe_unused]] auto condition = [&condition_calls] {
    ++condition_calls;
    return 0;
  };
  int stream_calls = 0;
  auto streamed = [&stream_calls] {
    ++stream_calls;
    return 0;
  };

  DCHECK(condition()) << streamed();

  EXPECT_EQ(condition_calls, 0);
  EXPECT_EQ(stream_calls, 1);
}

#endif

// ---------------------------------------------------------------------------
// 4. 失败路径：只能在子进程里观察
// ---------------------------------------------------------------------------

#if GTEST_HAS_DEATH_TEST

// A failed check aborts the process (base/check.cc:20), so asserting on it
// means letting it happen somewhere that is not us. gtest's death test forks
// and matches the child's stderr, which is where the default logger writes.
//
// The matchers avoid regex metacharacters on purpose: gtest uses POSIX extended
// regex on POSIX but its own simpler engine on Windows, and the two disagree
// about escaping.

TEST(CheckDeathTest, FailingCheckAborts) {
  EXPECT_DEATH(CHECK(false), "Check failed");
}

// #condition: the message names the failing expression, not just the file.
TEST(CheckDeathTest, MessageNamesTheCondition) {
  const bool ready = false;
  EXPECT_DEATH(CHECK(ready), "Check failed: ready");
}

// A helper rather than an inline statement: the comma in CHECK_EQ(a, b) would
// otherwise be read as EXPECT_DEATH's own argument separator.
void FailingComparison() {
  const int a = 1;
  const int b = 2;
  CHECK_EQ(a, b);
}

// Every CHECK_* comparison is CHECK() underneath, so a false one reaches the
// same abort path.
TEST(CheckDeathTest, FailingComparisonAborts) {
  EXPECT_DEATH(FailingComparison(), "Check failed");
}

// NOTREACHED() is always on -- there is no DNOTREACHED(). "This path ends
// here" is not something a release build should quietly let through.
TEST(CheckDeathTest, NotReachedAborts) {
  EXPECT_DEATH(NOTREACHED(), "NOTREACHED");
}

// The file:line in the text is NOTREACHED()'s own doing: __FILE__/__LINE__
// expand at the call site, while the LOG(FATAL) carrying them is emitted from
// base/check.cc. Plain CHECK() has no equivalent.
TEST(CheckDeathTest, NotReachedNamesItsCallSite) {
  EXPECT_DEATH(NOTREACHED(), "check_unittest");
}

// The workaround the previous test implies, pinned: stream the location in
// yourself. Copy this at CHECK() call sites whose failure you expect to have to
// diagnose from a log alone.
TEST(CheckDeathTest, CallerSuppliedContextReachesTheLog) {
  EXPECT_DEATH(CHECK(false) << " at " << __FILE__ << ":" << __LINE__,
               "check_unittest");
}

#endif  // GTEST_HAS_DEATH_TEST

}  // namespace
}  // namespace avbase::base
