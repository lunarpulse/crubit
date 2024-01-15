// Part of the Crubit project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "nullability/pointer_nullability.h"

#include <memory>

#include "nullability/type_nullability.h"
#include "clang/AST/Type.h"
#include "clang/Analysis/FlowSensitive/DataflowAnalysisContext.h"
#include "clang/Analysis/FlowSensitive/Formula.h"
#include "clang/Analysis/FlowSensitive/Value.h"
#include "clang/Analysis/FlowSensitive/WatchedLiteralsSolver.h"
#include "clang/Basic/Specifiers.h"
#include "third_party/llvm/llvm-project/third-party/unittest/googletest/include/gtest/gtest.h"

namespace clang::tidy::nullability {
namespace {

using dataflow::Arena;
using dataflow::Formula;

class NullabilityPropertiesTest : public ::testing::Test {
 protected:
  dataflow::PointerValue &makePointer(PointerTypeNullability N) {
    auto &P = Env.create<dataflow::PointerValue>(
        DACtx.createStorageLocation(QualType()));
    initPointerNullState(P, DACtx, N);
    return P;
  }

  dataflow::PointerValue &makeNullPointer() {
    auto &P = Env.create<dataflow::PointerValue>(
        DACtx.createStorageLocation(QualType()));
    initNullPointer(P, DACtx);
    return P;
  }

  dataflow::DataflowAnalysisContext DACtx = dataflow::DataflowAnalysisContext(
      std::make_unique<dataflow::WatchedLiteralsSolver>());
  dataflow::Environment Env = dataflow::Environment(DACtx);
};

TEST_F(NullabilityPropertiesTest, Test) {
  auto &A = DACtx.arena();

  EXPECT_TRUE(isNullable(makeNullPointer(), Env));

  {
    auto &NullableButNotNull = makePointer(NullabilityKind::Nullable);
    EXPECT_TRUE(isNullable(NullableButNotNull, Env));
    auto *IsNull = getPointerNullState(NullableButNotNull).IsNull;
    ASSERT_NE(IsNull, nullptr);
    Env.assume(A.makeNot(*IsNull));
    EXPECT_FALSE(isNullable(NullableButNotNull, Env));
  }

  {
    auto &NullableAndNull = makePointer(NullabilityKind::Nullable);
    auto *IsNull = getPointerNullState(NullableAndNull).IsNull;
    ASSERT_NE(IsNull, nullptr);
    Env.assume(*IsNull);
    EXPECT_TRUE(isNullable(NullableAndNull, Env));
  }

  {
    auto &NonnullAndNotNull = makePointer(NullabilityKind::NonNull);
    EXPECT_FALSE(isNullable(NonnullAndNotNull, Env));

    auto *IsNull = getPointerNullState(NonnullAndNotNull).IsNull;
    ASSERT_NE(IsNull, nullptr);

    // `IsNull` should not just be provably false but an actual false literal.
    ASSERT_EQ(IsNull, &A.makeLiteral(false));

    // Assuming the pointer is non-null is a no-op, but make sure it doesn't
    // change the result of `isNullable()`.
    Env.assume(A.makeNot(*IsNull));
    EXPECT_FALSE(isNullable(NonnullAndNotNull, Env));
  }

  {
    // This is a little surprising: if a pointer comes from a non-null source
    // but is dynamically discovered to be definitely null, we still don't
    // consider it nullable.
    auto &NonnullAndNull = makePointer(NullabilityKind::NonNull);
    auto *IsNull = getPointerNullState(NonnullAndNull).IsNull;
    ASSERT_NE(IsNull, nullptr);
    Env.assume(*IsNull);
    EXPECT_FALSE(isNullable(NonnullAndNull, Env));
  }
}

TEST_F(NullabilityPropertiesTest, IsNullableAdditionalConstraints) {
  auto &P = makePointer(NullabilityKind::Nullable);
  EXPECT_TRUE(isNullable(P, Env));
  auto *IsNull = getPointerNullState(P).IsNull;
  ASSERT_NE(IsNull, nullptr);
  auto *NotNull = &DACtx.arena().makeNot(*IsNull);
  EXPECT_FALSE(isNullable(P, Env, NotNull));
}

TEST_F(NullabilityPropertiesTest, GetNullabilityAdditionalConstraints) {
  auto &P = makePointer(NullabilityKind::Nullable);
  EXPECT_EQ(getNullability(P, Env), NullabilityKind::Nullable);
  auto *IsNull = getPointerNullState(P).IsNull;
  ASSERT_NE(IsNull, nullptr);
  auto *NotNull = &DACtx.arena().makeNot(*IsNull);
  EXPECT_EQ(getNullability(P, Env, NotNull), NullabilityKind::NonNull);
}

TEST_F(NullabilityPropertiesTest, InitNullabilityPropertiesWithFormulas) {
  auto &P = Env.create<dataflow::PointerValue>(
      DACtx.createStorageLocation(QualType()));

  Arena &A = DACtx.arena();
  const Formula &FromNullable = A.makeAtomRef(A.makeAtom());
  const Formula &IsNull = A.makeAtomRef(A.makeAtom());

  initPointerNullState(P, DACtx, PointerNullState{&FromNullable, &IsNull});
  ASSERT_EQ(getPointerNullState(P).FromNullable, &FromNullable);
  ASSERT_EQ(getPointerNullState(P).IsNull, &IsNull);
}

TEST_F(NullabilityPropertiesTest, InitNullabilityPropertiesWithTop) {
  auto &P = Env.create<dataflow::PointerValue>(
      DACtx.createStorageLocation(QualType()));

  initPointerNullState(P, DACtx, PointerNullState{nullptr, nullptr});
  ASSERT_EQ(getPointerNullState(P).FromNullable, nullptr);
  ASSERT_EQ(getPointerNullState(P).IsNull, nullptr);
}

}  // namespace
}  // namespace clang::tidy::nullability
