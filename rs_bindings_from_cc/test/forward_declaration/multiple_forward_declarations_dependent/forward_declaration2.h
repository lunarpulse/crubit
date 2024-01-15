// Part of the Crubit project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef THIRD_PARTY_CRUBIT_RS_BINDINGS_FROM_CC_TEST_FORWARD_DECLARATION_MULTIPLE_FORWARD_DECLARATIONS_INDEPENDENT_FORWARD_DECLARATION2_H_
#define THIRD_PARTY_CRUBIT_RS_BINDINGS_FROM_CC_TEST_FORWARD_DECLARATION_MULTIPLE_FORWARD_DECLARATIONS_INDEPENDENT_FORWARD_DECLARATION2_H_

#include "rs_bindings_from_cc/test/forward_declaration/multiple_forward_declarations_dependent/forward_declaration1.h"

struct A;
inline A* IdentityPtr2(A* ptr) { return ptr; }
inline A& IdentityRef2(A& a) { return a; }

#endif  // THIRD_PARTY_CRUBIT_RS_BINDINGS_FROM_CC_TEST_FORWARD_DECLARATION_MULTIPLE_FORWARD_DECLARATIONS_INDEPENDENT_FORWARD_DECLARATION2_H_
